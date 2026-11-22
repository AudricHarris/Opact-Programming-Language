//==========================================================================//
//  Structs.cpp  -  structure literals and member access                    //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(StructCreateExpr& node) {
	lastValue = nullptr;

	auto* id = dynamic_cast<IdentifierExpr*>(node.structObj.get());
	if (!id) {
		error("Only direct structure creation by name is supported");
		return;
	}
	if (!currentFn) {
		error("Structure creation outside of a function");
		return;
	}

	auto it = structs.find(id->name);
	if (it == structs.end()) {
		error("Unknown structure type '" + id->name + "'");
		return;
	}
	const StructInfo& info = it->second;
	llvm::StructType* llvmStructTy = info.type;

	if (llvmStructTy->isOpaque()) {
		error("Structure '" + id->name + "' is used before its definition");
		return;
	}

	if (llvmStructTy->getNumElements() != node.arguments.size()) {
		error("Structure '" + id->name + "' expects " +
				std::to_string(llvmStructTy->getNumElements()) +
				" argument(s), but got " + std::to_string(node.arguments.size()));
		return;
	}

	// `Tile[20][20] name` given to a '?' array field: calloc the storage and
	// build the { data, dim0, dim1, ... } descriptor.
	auto emitDynAlloc = [&](ArrayAllocExpr& alloc, llvm::Type* dynTy) -> llvm::Value* {
		const DynArrayInfo* dyn = dynInfo(dynTy);
		if (hasUnknownDim(alloc.type)) {
			error("Cannot allocate an array with an unknown ('?') dimension: give every dimension a size");
			return nullptr;
		}
		if (alloc.type.dim.size() != dyn->rank) {
			error("Array has " + std::to_string(alloc.type.dim.size()) + " dimension(s) but the field expects " +
					std::to_string(dyn->rank));
			return nullptr;
		}
		TypeDesc elemDesc = alloc.type;
		elemDesc.dim.clear();
		if (mapType(elemDesc) != dyn->elemTy) {
			error("Array element type does not match the field's element type");
			return nullptr;
		}
		if (!dyn->elemTy->isSized()) {
			error("Array element type is not defined yet");
			return nullptr;
		}

		uint64_t count = 1;
		for (int d : alloc.type.dim) count *= static_cast<uint64_t>(d);
		uint64_t elemSize = module->getDataLayout().getTypeAllocSize(dyn->elemTy);

		llvm::Type* i64 = builder.getInt64Ty();
		auto callocFn = module->getOrInsertFunction("calloc",
				llvm::FunctionType::get(builder.getPtrTy(), { i64, i64 }, false));
		llvm::Value* data = builder.CreateCall(callocFn,
				{ builder.getInt64(count), builder.getInt64(elemSize) }, "dyn_alloc");

		llvm::Value* desc = llvm::PoisonValue::get(dynTy);
		desc = builder.CreateInsertValue(desc, data, 0);
		for (size_t k = 0; k < alloc.type.dim.size(); ++k)
			desc = builder.CreateInsertValue(desc, builder.getInt64(static_cast<uint64_t>(alloc.type.dim[k])),
					static_cast<unsigned>(k + 1));
		return desc;
	};

	llvm::AllocaInst* structAlloca = createEntryAlloca(currentFn, llvmStructTy, id->name + "_tmp");
	builder.CreateStore(llvm::Constant::getNullValue(llvmStructTy), structAlloca);

	for (size_t i = 0; i < node.arguments.size(); ++i) {
		llvm::Type* fieldTy = llvmStructTy->getElementType(i);
		bool isRef = i < info.fieldIsRef.size() && info.fieldIsRef[i];
		bool isUns = i < info.fieldIsUnsigned.size() && info.fieldIsUnsigned[i] && fieldTy->isIntegerTy();

		llvm::Value* argVal = nullptr;
		bool isSkip = false;
		if (auto* skipId = dynamic_cast<IdentifierExpr*>(node.arguments[i].get()); skipId && skipId->name == "_") {
			isSkip = true;
		}

		if (isSkip) {
			argVal = llvm::Constant::getNullValue(fieldTy);
		} else if (auto* alloc = dynamic_cast<ArrayAllocExpr*>(node.arguments[i].get());
				alloc && dynInfo(fieldTy)) {
			argVal = emitDynAlloc(*alloc, fieldTy);
			if (!argVal) return;
		} else {
			argVal = isUns
				? emitUnsignedValue(node.arguments[i].get(), fieldTy, "field '" + info.fieldNames[i] + "'")
				: (isRef ? emitRef(node.arguments[i].get()) : emit(node.arguments[i].get()));
			if (!argVal) return;

			argVal = coerce(argVal, fieldTy);
			if (!argVal) return;
		}
		llvm::Value* fieldPtr = builder.CreateStructGEP(llvmStructTy, structAlloca, i,
				id->name + "_field_" + std::to_string(i));
		builder.CreateStore(argVal, fieldPtr);
	}

	lastValue = structAlloca;
}

void CodeGenerator::visit(MemberAccessExpr& node) {
	lastValue = nullptr;

	if (node.memberName == "length") {
		llvm::Type* arrTy = nullptr;
		llvm::Value* arrAddr = nullptr;
		size_t dimIdx = 0;

		auto resolveDynFromPtr = [&](llvm::Value* addr, llvm::Type* ty) {
			if (ty && dynInfo(ty)) {
				arrAddr = addr;
				arrTy = ty;
				return;
			}
			if (ty && ty->isPointerTy() && addr) {
				for (const auto& kv : g_dynArrays) {
					auto* candTy = const_cast<llvm::Type*>(kv.first);
					arrAddr = builder.CreateLoad(builder.getPtrTy(), addr, "dyn_ptr_load");
					arrTy = candTy;
					return;
				}
			}
		};

		if (auto* id = dynamic_cast<IdentifierExpr*>(node.object.get())) {
			VarInfo* var = lookupVar(id->name);
			if (var) {
				arrTy = var->type;
				arrAddr = var->addr;
				if (var->pointee) {
					arrAddr = builder.CreateLoad(builder.getPtrTy(), arrAddr, id->name + "_ref");
					arrTy = var->pointee;
				} else {
					resolveDynFromPtr(arrAddr, arrTy);
				}
			}
		}
		else if (auto* aie = dynamic_cast<ArrayIndexExpr*>(node.object.get())) {
			if (auto* baseId = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
				VarInfo* var = lookupVar(baseId->name);
				if (var) {
					arrTy = var->type;
					arrAddr = var->addr;
					if (var->pointee) {
						arrAddr = builder.CreateLoad(builder.getPtrTy(), arrAddr, baseId->name + "_ref");
						arrTy = var->pointee;
					} else {
						resolveDynFromPtr(arrAddr, arrTy);
					}
				}
			} else if (auto* baseMa = dynamic_cast<MemberAccessExpr*>(aie->array.get())) {
				llvm::Value* tmpAddr = nullptr;
				llvm::Type* tmpTy = nullptr;
				if (emitFieldAddr(*baseMa, tmpAddr, tmpTy)) {
					arrTy = tmpTy;
					arrAddr = tmpAddr;
				}
			}

			if (arrTy && arrAddr) {
				size_t remIndices = aie->indices.size();
				for (size_t i = 0; i < aie->indices.size() && arrTy; ++i) {
					if (const DynArrayInfo* dyn = dynInfo(arrTy)) {
						if (remIndices < dyn->rank) {
							dimIdx = remIndices;
							break;
						}
						arrTy = dyn->elemTy;
						remIndices -= dyn->rank;
						i += dyn->rank - 1;
					} else if (arrTy->isArrayTy()) {
						arrTy = llvm::cast<llvm::ArrayType>(arrTy)->getElementType();
						--remIndices;
					} else {
						break;
					}
				}
			}
		}
		else if (auto* baseMa = dynamic_cast<MemberAccessExpr*>(node.object.get())) {
			llvm::Value* tmpAddr = nullptr;
			llvm::Type* tmpTy = nullptr;
			if (emitFieldAddr(*baseMa, tmpAddr, tmpTy)) {
				arrTy = tmpTy;
				arrAddr = tmpAddr;
				if (!dynInfo(arrTy)) resolveDynFromPtr(arrAddr, arrTy);
			}
		}

		if (arrTy && arrAddr) {
			if (const DynArrayInfo* dyn = dynInfo(arrTy)) {
				if (dimIdx < dyn->rank) {
					llvm::Value* dim = builder.CreateLoad(builder.getInt64Ty(),
							builder.CreateStructGEP(llvm::cast<llvm::StructType>(arrTy), arrAddr,
								static_cast<unsigned>(dimIdx + 1)), "dyn_len");
					lastValue = builder.CreateTrunc(dim, builder.getInt32Ty(), "len_i32");
					return;
				}
				arrTy = nullptr;
			}
		}

		if (arrTy && arrTy->isArrayTy()) {
			uint64_t len = llvm::cast<llvm::ArrayType>(arrTy)->getNumElements();
			lastValue = builder.getInt32(static_cast<uint32_t>(len));
			return;
		}

		llvm::Value* objVal = emit(node.object.get());
		if (objVal && objVal->getType()->isPointerTy()) {
			auto sizeTy = builder.getInt64Ty();
			auto strlenFn = module->getOrInsertFunction("strlen", llvm::FunctionType::get(sizeTy, { builder.getPtrTy() }, false));
			llvm::Value* strPtr = builder.CreatePointerCast(objVal, builder.getPtrTy(), "str_cast");
			llvm::Value* len64 = builder.CreateCall(strlenFn, { strPtr }, "strlen_tmp");
			lastValue = builder.CreateTrunc(len64, builder.getInt32Ty(), "len_i32");
			return;
		}
	}

	llvm::Value* addr = nullptr;
	llvm::Type* ty = nullptr;
	if (!emitFieldAddr(node, addr, ty)) return;

	lastValue = builder.CreateLoad(ty, addr, node.memberName);
}
