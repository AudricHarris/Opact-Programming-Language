//==========================================================================//
//  Enums.cpp  -  tagged enums, variant access, match, and switch           //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace codegen_detail;

namespace {

bool hasTerminator(const llvm::BasicBlock* bb) {
	return bb && !bb->empty() && bb->back().isTerminator();
}

const CodeGenerator::EnumInfo* findEnumByStorageType(
		const std::unordered_map<std::string, CodeGenerator::EnumInfo>& enums,
		llvm::Type* ty) {
	auto* st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
	if (!st) return nullptr;
	for (const auto& kv : enums) {
		if (kv.second.storageType == st) {
			return &kv.second;
		}
	}
	return nullptr;
}

const CodeGenerator::EnumVariantInfo* findVariant(
		const CodeGenerator::EnumInfo& einfo,
		const std::string& variantName) {
	for (const auto& v : einfo.variants) {
		if (v.name == variantName) {
			return &v;
		}
	}
	return nullptr;
}

} // namespace

//==========================================================================//
//  Enum Declaration & Construction                                         //
//==========================================================================//

const CodeGenerator::EnumVariantInfo* CodeGenerator::findEnumVariant(
		const EnumInfo& einfo, const std::string& variantName) const {
	for (const auto& v : einfo.variants) {
		if (v.name == variantName) {
			return &v;
		}
	}
	return nullptr;
}

void CodeGenerator::visit(EnumDeclExpr& node) {
	lastValue = nullptr;

	uint64_t maxPayload = 0;
	int nextTag = 0;
	EnumInfo info;

	for (const auto& field : node.fields) {
		EnumVariantInfo vinfo;
		vinfo.name = field.name;
		vinfo.tag = field.explicitValue.value_or(nextTag);
		nextTag = vinfo.tag + 1;

		uint64_t offset = 0;
		for (const auto& attr : field.attributes) {
			llvm::Type* ty = mapType(attr.type);
			if (!ty || ty->isVoidTy()) continue;

			uint64_t align = module->getDataLayout().getABITypeAlign(ty).value();
			uint64_t size = module->getDataLayout().getTypeAllocSize(ty);
			if (align > 0) {
				offset = ((offset + align - 1) / align) * align;
			}
			vinfo.payloadTypes.push_back(ty);
			vinfo.payloadOffsets.push_back(offset);
			offset += size;
		}

		if (offset > maxPayload) {
			maxPayload = offset;
		}
		info.variants.push_back(std::move(vinfo));
	}

	info.payloadBytes = maxPayload;
	llvm::Type* tagTy = builder.getInt32Ty();
	llvm::Type* payloadSlotTy = llvm::ArrayType::get(builder.getInt8Ty(), maxPayload);
	info.storageType = llvm::StructType::create(
			context, { tagTy, payloadSlotTy }, "enum." + node.name);

	enums[node.name] = std::move(info);
}

void CodeGenerator::visit(EnumCreateExpr& node) {
	lastValue = nullptr;

	if (!currentFn) {
		error("Enum creation outside of a function");
		return;
	}

	auto eit = enums.find(node.enumName);
	if (eit == enums.end() || !eit->second.storageType) {
		error("Unknown enum type '" + node.enumName + "'");
		return;
	}

	const EnumInfo& einfo = eit->second;
	const EnumVariantInfo* vinfo = findVariant(einfo, node.variantName);
	if (!vinfo) {
		error("Unknown variant '" + node.variantName + "' for enum '" + node.enumName + "'");
		return;
	}

	if (vinfo->payloadTypes.size() != node.args.size()) {
		error("Variant '" + node.enumName + "::" + node.variantName + "' expects " +
				std::to_string(vinfo->payloadTypes.size()) + " argument(s), got " +
				std::to_string(node.args.size()));
		return;
	}

	llvm::AllocaInst* enumAlloca = createEntryAlloca(
			currentFn, einfo.storageType, node.enumName + "_tmp");
	builder.CreateStore(llvm::Constant::getNullValue(einfo.storageType), enumAlloca);

	// Store discriminant tag at index 0
	llvm::Value* tagPtr = builder.CreateStructGEP(
			einfo.storageType, enumAlloca, 0, "enum_tag_ptr");
	builder.CreateStore(builder.getInt32(vinfo->tag), tagPtr);

	// Store payload arguments at their byte offsets in payload array (index 1)
	if (!node.args.empty()) {
		llvm::Value* payloadArrPtr = builder.CreateStructGEP(
				einfo.storageType, enumAlloca, 1, "enum_payload_ptr");
		llvm::Type* payloadArrTy = llvm::ArrayType::get(builder.getInt8Ty(), einfo.payloadBytes);

		for (size_t i = 0; i < node.args.size(); ++i) {
			if (!node.args[i]) continue;
			llvm::Value* argVal = emit(node.args[i].get());
			if (!argVal) return;

			llvm::Type* targetTy = vinfo->payloadTypes[i];
			if ((targetTy->isStructTy() || targetTy->isArrayTy()) && argVal->getType()->isPointerTy()) {
				argVal = builder.CreateLoad(targetTy, argVal, "enum_arg_load");
			}
			argVal = coerce(argVal, targetTy);
			if (!argVal) return;

			llvm::Value* bytePtr = builder.CreateConstGEP2_64(
					payloadArrTy, payloadArrPtr, 0, vinfo->payloadOffsets[i],
					"payload_off_" + std::to_string(i));
			llvm::Value* typedPtr = builder.CreatePointerCast(
					bytePtr, builder.getPtrTy(), "payload_typed_ptr");
			builder.CreateStore(argVal, typedPtr);
		}
	}

	lastValue = enumAlloca;
}

//==========================================================================//
//  Payload Extraction                                                      //
//==========================================================================//

void CodeGenerator::visit(EnumGetExpr& node) {
	lastValue = nullptr;

	if (!node.target || !currentFn) {
		error("Invalid enum payload access");
		return;
	}

	const EnumInfo* einfo = nullptr;
	if (auto* id = dynamic_cast<IdentifierExpr*>(node.target.get())) {
		auto eit = enums.find(id->name);
		if (eit != enums.end()) {
			einfo = &eit->second;
			if (!node.expectedVariant && node.kind == EnumGetKind::NamedField) {
				if (const EnumVariantInfo* vinfo = findVariant(*einfo, node.fieldName)) {
					llvm::AllocaInst* enumAlloca = createEntryAlloca(
							currentFn, einfo->storageType, id->name + "_val");
					builder.CreateStore(llvm::Constant::getNullValue(einfo->storageType), enumAlloca);
					llvm::Value* tagPtr = builder.CreateStructGEP(
							einfo->storageType, enumAlloca, 0, "enum_tag_ptr");
					builder.CreateStore(builder.getInt32(vinfo->tag), tagPtr);
					lastValue = enumAlloca;
					return;
				}
			}
		}
	}

	llvm::Value* targetVal = emit(node.target.get());
	if (!targetVal) return;

	if (!einfo) {
		if (auto* id = dynamic_cast<IdentifierExpr*>(node.target.get())) {
			if (VarInfo* var = lookupVar(id->name)) {
				llvm::Type* ty = var->pointee ? var->pointee : var->type;
				einfo = findEnumByStorageType(enums, ty);
			}
		}
		if (!einfo) {
			einfo = findEnumByStorageType(enums, targetVal->getType());
		}
		if (!einfo && !enums.empty()) {
			einfo = &enums.begin()->second;
		}
	}

	if (!einfo || !einfo->storageType) {
		error("Could not resolve enum storage type for payload access");
		return;
	}

	llvm::Value* enumPtr = targetVal;
	if (!enumPtr->getType()->isPointerTy()) {
		llvm::AllocaInst* tmp = createEntryAlloca(currentFn, einfo->storageType, "enum_get_tmp");
		builder.CreateStore(enumPtr, tmp);
		enumPtr = tmp;
	}

	const EnumVariantInfo* vinfo = nullptr;
	if (node.expectedVariant) {
		vinfo = findVariant(*einfo, *node.expectedVariant);
	} else if (!einfo->variants.empty()) {
		vinfo = &einfo->variants.front();
	}

	if (!vinfo || node.fieldIndex >= vinfo->payloadTypes.size()) {
		error("Invalid payload field access on enum");
		return;
	}

	llvm::Type* payloadArrTy = llvm::ArrayType::get(builder.getInt8Ty(), einfo->payloadBytes);
	llvm::Value* payloadArrPtr = builder.CreateStructGEP(
			einfo->storageType, enumPtr, 1, "enum_payload_ptr");
	llvm::Value* bytePtr = builder.CreateConstGEP2_64(
			payloadArrTy, payloadArrPtr, 0, vinfo->payloadOffsets[node.fieldIndex],
			"payload_byte_ptr");
	llvm::Type* fieldTy = vinfo->payloadTypes[node.fieldIndex];
	llvm::Value* typedPtr = builder.CreatePointerCast(
			bytePtr, builder.getPtrTy(), "payload_field_ptr");

	lastValue = builder.CreateLoad(fieldTy, typedPtr, "enum_payload_val");
}

//==========================================================================//
//  Match & Switch                                                          //
//==========================================================================//

void CodeGenerator::visit(MatchExpr& node) {
	lastValue = nullptr;

	if (!node.scrutinee || !currentFn) {
		error("Invalid match expression");
		return;
	}

	llvm::Value* scrVal = emit(node.scrutinee.get());
	if (!scrVal) return;

	const EnumInfo* einfo = nullptr;
	if (auto* id = dynamic_cast<IdentifierExpr*>(node.scrutinee.get())) {
		if (VarInfo* var = lookupVar(id->name)) {
			llvm::Type* ty = var->pointee ? var->pointee : var->type;
			einfo = findEnumStorageType(ty);
		}
	} else if (auto* create = dynamic_cast<EnumCreateExpr*>(node.scrutinee.get())) {
		auto eit = enums.find(create->enumName);
		if (eit != enums.end()) einfo = &eit->second;
	}

	if (!einfo) {
		einfo = findEnumStorageType(scrVal->getType());
	}
	if (!einfo && !enums.empty()) {
		einfo = &enums.begin()->second;
	}

	if (!einfo || !einfo->storageType) {
		error("Match scrutinee is not a known enum");
		return;
	}

	llvm::Value* enumPtr = scrVal;
	if (!enumPtr->getType()->isPointerTy()) {
		llvm::AllocaInst* tmp = createEntryAlloca(currentFn, einfo->storageType, "match_scr_tmp");
		builder.CreateStore(enumPtr, tmp);
		enumPtr = tmp;
	}

	llvm::BasicBlock* defaultBB = llvm::BasicBlock::Create(context, "match.default", currentFn);
	llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context, "match.end", currentFn);

	llvm::Value* tagPtr = builder.CreateStructGEP(
			einfo->storageType, enumPtr, 0, "match_tag_ptr");
	llvm::Value* tagVal = builder.CreateLoad(builder.getInt32Ty(), tagPtr, "match_tag");

	llvm::SwitchInst* sw = builder.CreateSwitch(
			tagVal, defaultBB, static_cast<unsigned>(node.arms.size()));

	// Track every predecessor block jumping to `mergeBB` and its value (or nullptr).
	std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> predValues;
	bool hasWildcard = false;

	auto finishToMerge = [&](llvm::Value* val) {
		llvm::BasicBlock* curBB = builder.GetInsertBlock();
		if (curBB && (curBB->empty() || !curBB->back().isTerminator())) {
			builder.CreateBr(mergeBB);
			predValues.emplace_back(curBB, val);
		}
	};

	for (size_t i = 0; i < node.arms.size(); ++i) {
		const MatchArm& arm = node.arms[i];
		llvm::BasicBlock* armBB = llvm::BasicBlock::Create(
				context, "match.arm." + std::to_string(i), currentFn);

		if (arm.pattern.kind == MatchPattern::Kind::Wildcard) {
			hasWildcard = true;
			if (defaultBB->empty() || !defaultBB->back().isTerminator()) {
				builder.SetInsertPoint(defaultBB);
				builder.CreateBr(armBB);
			}
		} else {
			if (const EnumVariantInfo* vinfo = findEnumVariant(*einfo, arm.pattern.variantName)) {
				sw->addCase(builder.getInt32(vinfo->tag), armBB);
			}
		}

		builder.SetInsertPoint(armBB);

		if (arm.pattern.kind == MatchPattern::Kind::Variant && !arm.pattern.bindingNames.empty()) {
			if (const EnumVariantInfo* vinfo = findEnumVariant(*einfo, arm.pattern.variantName)) {
				llvm::Type* payloadArrTy = llvm::ArrayType::get(builder.getInt8Ty(), einfo->payloadBytes);
				llvm::Value* payloadArrPtr = builder.CreateStructGEP(
						einfo->storageType, enumPtr, 1, "arm_payload_ptr");

				size_t n = std::min(arm.pattern.bindingNames.size(), vinfo->payloadTypes.size());
				for (size_t b = 0; b < n; ++b) {
					const std::string& bName = arm.pattern.bindingNames[b];
					if (bName == "_") continue;

					llvm::Type* bTy = vinfo->payloadTypes[b];
					llvm::Value* bytePtr = builder.CreateConstGEP2_64(
							payloadArrTy, payloadArrPtr, 0, vinfo->payloadOffsets[b],
							"bind_byte_" + bName);
					llvm::Value* typedPtr = builder.CreatePointerCast(
							bytePtr, builder.getPtrTy(), "bind_typed_" + bName);
					llvm::Value* loaded = builder.CreateLoad(bTy, typedPtr, "bind_val_" + bName);

					llvm::AllocaInst* slot = createEntryAlloca(currentFn, bTy, bName);
					builder.CreateStore(loaded, slot);

					llvm::StructType* bStruct = llvm::dyn_cast<llvm::StructType>(bTy);
					if (!scopes.empty()) {
						scopes.back()[bName] = VarInfo{slot, bTy, bStruct, nullptr, false, nullptr};
					}
				}
			}
		}

		llvm::Value* bodyVal = arm.body ? emit(arm.body.get()) : nullptr;
		finishToMerge(bodyVal);
	}

	if (!hasWildcard && (defaultBB->empty() || !defaultBB->back().isTerminator())) {
		builder.SetInsertPoint(defaultBB);
		builder.CreateBr(mergeBB);
		predValues.emplace_back(defaultBB, nullptr);
	}

	builder.SetInsertPoint(mergeBB);

	llvm::Type* phiTy = nullptr;
	bool hasAnyVal = false;
	bool sameTy = true;

	for (const auto& [bb, val] : predValues) {
		(void)bb;
		if (!val || val->getType()->isVoidTy()) continue;
		if (!phiTy) {
			phiTy = val->getType();
			hasAnyVal = true;
		} else if (val->getType() != phiTy) {
			sameTy = false;
			break;
		}
	}

	if (hasAnyVal && sameTy && phiTy && !phiTy->isVoidTy()) {
		llvm::PHINode* phi = builder.CreatePHI(
				phiTy, static_cast<unsigned>(predValues.size()), "match_phi");
		for (const auto& [bb, val] : predValues) {
			llvm::Value* inVal = (val && val->getType() == phiTy)
					? val
					: llvm::PoisonValue::get(phiTy);
			phi->addIncoming(inVal, bb);
		}
		lastValue = phi;
	}
}

void CodeGenerator::visit(SwitchExpr& node) {
	lastValue = nullptr;

	if (!node.value || !currentFn) {
		error("Invalid switch expression");
		return;
	}

	llvm::Value* condVal = emit(node.value.get());
	if (!condVal) return;

	llvm::BasicBlock* defaultBB = llvm::BasicBlock::Create(context, "switch.default", currentFn);
	llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context, "switch.end", currentFn);

	llvm::SwitchInst* sw = builder.CreateSwitch(
			condVal, defaultBB, static_cast<unsigned>(node.cases.size()));

	const SwitchCase* wildcardCase = nullptr;
	std::vector<const SwitchCase*> normalCases;
	for (const auto& c : node.cases) {
		if (c.isWildcard) {
			wildcardCase = &c;
		} else {
			normalCases.push_back(&c);
		}
	}

	// Track every predecessor block jumping to `mergeBB` and its value (or nullptr).
	std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> predValues;

	auto finishToMerge = [&](llvm::Value* val) {
		llvm::BasicBlock* curBB = builder.GetInsertBlock();
		if (curBB && (curBB->empty() || !curBB->back().isTerminator())) {
			builder.CreateBr(mergeBB);
			predValues.emplace_back(curBB, val);
		}
	};

	for (size_t i = 0; i < normalCases.size(); ++i) {
		const SwitchCase* c = normalCases[i];
		llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(
				context, "switch.case." + std::to_string(i), currentFn);

		for (const auto& lbl : c->labels) {
			if (!lbl) continue;
			llvm::Value* lblVal = emit(lbl.get());
			llvm::Value* coerced = lblVal ? coerce(lblVal, condVal->getType()) : nullptr;
			if (auto* cInt = llvm::dyn_cast_or_null<llvm::ConstantInt>(coerced)) {
				sw->addCase(cInt, caseBB);
			}
		}

		builder.SetInsertPoint(caseBB);
		llvm::Value* bodyVal = c->body ? emit(c->body.get()) : nullptr;
		finishToMerge(bodyVal);
	}

	builder.SetInsertPoint(defaultBB);
	if (wildcardCase && wildcardCase->body) {
		llvm::Value* bodyVal = emit(wildcardCase->body.get());
		finishToMerge(bodyVal);
	} else if (defaultBB->empty() || !defaultBB->back().isTerminator()) {
		builder.CreateBr(mergeBB);
		predValues.emplace_back(defaultBB, nullptr);
	}

	builder.SetInsertPoint(mergeBB);

	llvm::Type* phiTy = nullptr;
	bool hasAnyVal = false;
	bool sameTy = true;

	for (const auto& [bb, val] : predValues) {
		(void)bb;
		if (!val || val->getType()->isVoidTy()) continue;
		if (!phiTy) {
			phiTy = val->getType();
			hasAnyVal = true;
		} else if (val->getType() != phiTy) {
			sameTy = false;
			break;
		}
	}

	if (hasAnyVal && sameTy && phiTy && !phiTy->isVoidTy()) {
		llvm::PHINode* phi = builder.CreatePHI(
				phiTy, static_cast<unsigned>(predValues.size()), "switch_phi");
		for (const auto& [bb, val] : predValues) {
			llvm::Value* inVal = (val && val->getType() == phiTy)
					? val
					: llvm::PoisonValue::get(phiTy);
			phi->addIncoming(inVal, bb);
		}
		lastValue = phi;
	}
}

const CodeGenerator::EnumInfo* CodeGenerator::findEnumStorageType(llvm::Type* ty) const {
	auto* st = llvm::dyn_cast_or_null<llvm::StructType>(ty);
	if (!st) return nullptr;
	for (const auto& kv : enums) {
		if (kv.second.storageType == st) {
			return &kv.second;
		}
	}
	return nullptr;
}

llvm::Value* CodeGenerator::emitEnumComparison(BinaryOp op, llvm::Value* lhs, llvm::Value* rhs) {
	if (!currentFn || !lhs || !rhs) return nullptr;

	const EnumInfo* einfo = findEnumStorageType(lhs->getType());
	if (!einfo) {
		einfo = findEnumStorageType(rhs->getType());
	}
	if (!einfo || !einfo->storageType) {
		error("Cannot compare non-enum values with enum comparison");
		return nullptr;
	}

	auto toEnumPtr = [&](llvm::Value* v, const char* name) -> llvm::Value* {
		if (v->getType()->isPointerTy()) return v;
		llvm::AllocaInst* tmp = createEntryAlloca(currentFn, einfo->storageType, name);
		builder.CreateStore(v, tmp);
		return tmp;
	};

	llvm::Value* lhsPtr = toEnumPtr(lhs, "enum_cmp_lhs");
	llvm::Value* rhsPtr = toEnumPtr(rhs, "enum_cmp_rhs");

	llvm::Value* lTagPtr = builder.CreateStructGEP(einfo->storageType, lhsPtr, 0, "l_tag_ptr");
	llvm::Value* rTagPtr = builder.CreateStructGEP(einfo->storageType, rhsPtr, 0, "r_tag_ptr");
	llvm::Value* lTag = builder.CreateLoad(builder.getInt32Ty(), lTagPtr, "l_tag");
	llvm::Value* rTag = builder.CreateLoad(builder.getInt32Ty(), rTagPtr, "r_tag");

	llvm::Value* tagsEq = builder.CreateICmpEQ(lTag, rTag, "enum_tags_eq");

	if (einfo->payloadBytes > 0) {
		llvm::ArrayType* payloadArrTy = llvm::ArrayType::get(builder.getInt8Ty(), einfo->payloadBytes);
		llvm::Value* lPayloadPtr = builder.CreateStructGEP(einfo->storageType, lhsPtr, 1, "l_payload_ptr");
		llvm::Value* rPayloadPtr = builder.CreateStructGEP(einfo->storageType, rhsPtr, 1, "r_payload_ptr");

		llvm::Value* payloadEq = builder.getTrue();
		for (uint64_t idx = 0; idx < einfo->payloadBytes; ++idx) {
			llvm::Value* lBytePtr = builder.CreateConstGEP2_64(
					payloadArrTy, lPayloadPtr, 0, idx, "l_b_ptr");
			llvm::Value* rBytePtr = builder.CreateConstGEP2_64(
					payloadArrTy, rPayloadPtr, 0, idx, "r_b_ptr");
			llvm::Value* lByte = builder.CreateLoad(builder.getInt8Ty(), lBytePtr, "l_b");
			llvm::Value* rByte = builder.CreateLoad(builder.getInt8Ty(), rBytePtr, "r_b");
			llvm::Value* bEq = builder.CreateICmpEQ(lByte, rByte, "b_eq");
			payloadEq = builder.CreateAnd(payloadEq, bEq, "payload_eq_acc");
		}
		tagsEq = builder.CreateAnd(tagsEq, payloadEq, "enum_full_eq");
	}

	if (op == BinaryOp::Eq) {
		return tagsEq;
	}
	return builder.CreateNot(tagsEq, "enum_ne");
}
