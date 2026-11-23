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

	// `Tile[20][20] name` (optionally `= {...}`) given to a '?' array field:
	// calloc the storage and build the { data, dim0, dim1, ... } descriptor.
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

		if (alloc.initExpr && *alloc.initExpr) {
			if (auto* arrLit = dynamic_cast<ArrayLiteralExpr*>(alloc.initExpr->get())) {
				size_t n = std::min<size_t>(arrLit->elements.size(), static_cast<size_t>(count));
				for (size_t idx = 0; idx < n; ++idx) {
					bool prevUnsigned = unsignedCtx;
					unsignedCtx = alloc.type.isUnsignedInteger();
					llvm::Value* elemVal = emit(arrLit->elements[idx].get());
					unsignedCtx = prevUnsigned;
					if (!elemVal) return nullptr;

					if ((dyn->elemTy->isStructTy() || dyn->elemTy->isArrayTy()) && elemVal->getType()->isPointerTy()) {
						elemVal = builder.CreateLoad(dyn->elemTy, elemVal, "dyn_init_elem_load");
					} else {
						elemVal = coerce(elemVal, dyn->elemTy);
						if (!elemVal) return nullptr;
					}

					llvm::Value* dstPtr = builder.CreateInBoundsGEP(
							dyn->elemTy, data, { builder.getInt64(idx) }, "dyn_init_dst");
					builder.CreateStore(elemVal, dstPtr);
				}
			} else {
				bool prevUnsigned = unsignedCtx;
				unsignedCtx = alloc.type.isUnsignedInteger();
				llvm::Value* initVal = emit(alloc.initExpr->get());
				unsignedCtx = prevUnsigned;
				if (!initVal) return nullptr;

				llvm::ArrayType* fixedTy = llvm::ArrayType::get(dyn->elemTy, count);
				if (initVal->getType()->isPointerTy()) {
					llvm::Value* loaded = builder.CreateLoad(fixedTy, initVal, "dyn_init_arr_load");
					llvm::AllocaInst* tmpArr = createEntryAlloca(currentFn, fixedTy, "dyn_init_arr_tmp");
					builder.CreateStore(loaded, tmpArr);
					for (uint64_t idx = 0; idx < count; ++idx) {
						llvm::Value* srcPtr = builder.CreateInBoundsGEP(
								fixedTy, tmpArr, { builder.getInt64(0), builder.getInt64(idx) }, "dyn_init_src");
						llvm::Value* elemVal = builder.CreateLoad(dyn->elemTy, srcPtr, "dyn_init_copy_load");
						llvm::Value* dstPtr = builder.CreateInBoundsGEP(
								dyn->elemTy, data, { builder.getInt64(idx) }, "dyn_init_dst");
						builder.CreateStore(elemVal, dstPtr);
					}
				}
			}
		}

		llvm::Value* desc = llvm::PoisonValue::get(dynTy);
		desc = builder.CreateInsertValue(desc, data, 0);
		for (size_t k = 0; k < alloc.type.dim.size(); ++k)
			desc = builder.CreateInsertValue(desc, builder.getInt64(static_cast<uint64_t>(alloc.type.dim[k])),
					static_cast<unsigned>(k + 1));
		return desc;
	};

	// Convert a fixed-size array value/pointer into a `%dynarray` descriptor.
	auto emitFixedArrayToDyn = [&](Expression* argExpr, llvm::Type* dynTy) -> llvm::Value* {
		const DynArrayInfo* dyn = dynInfo(dynTy);
		if (!dyn || !argExpr) return nullptr;

		llvm::Type* arrTyRaw = nullptr;
		if (auto* argId = dynamic_cast<IdentifierExpr*>(argExpr)) {
			if (VarInfo* var = lookupVar(argId->name)) {
				arrTyRaw = var->pointee ? var->pointee : var->type;
			}
		}

		llvm::Value* rawVal = emit(argExpr);
		if (!rawVal) return nullptr;

		auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(arrTyRaw);
		if (!arrTy && rawVal->getType()->isPointerTy()) {
			if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(rawVal->stripPointerCasts())) {
				arrTy = llvm::dyn_cast<llvm::ArrayType>(allocaInst->getAllocatedType());
			}
		}

		if (!arrTy) {
			return coerce(rawVal, dynTy);
		}

		uint64_t count = arrTy->getNumElements();
		uint64_t elemSize = module->getDataLayout().getTypeAllocSize(dyn->elemTy);
		llvm::Type* i64 = builder.getInt64Ty();

		auto callocFn = module->getOrInsertFunction("calloc",
				llvm::FunctionType::get(builder.getPtrTy(), { i64, i64 }, false));
		llvm::Value* data = builder.CreateCall(callocFn,
				{ builder.getInt64(count), builder.getInt64(elemSize) }, "arr_to_dyn_data");

		llvm::AllocaInst* srcAlloca = createEntryAlloca(currentFn, arrTy, "arr_to_dyn_src_tmp");
		if (rawVal->getType()->isPointerTy()) {
			llvm::Value* loadedArr = builder.CreateLoad(arrTy, rawVal, "arr_to_dyn_load");
			builder.CreateStore(loadedArr, srcAlloca);
		} else {
			builder.CreateStore(rawVal, srcAlloca);
		}

		for (uint64_t idx = 0; idx < count; ++idx) {
			llvm::Value* srcPtr = builder.CreateInBoundsGEP(
					arrTy, srcAlloca, { builder.getInt64(0), builder.getInt64(idx) }, "arr_to_dyn_elem_ptr");
			llvm::Value* elemVal = builder.CreateLoad(arrTy->getElementType(), srcPtr, "arr_to_dyn_elem");
			elemVal = coerce(elemVal, dyn->elemTy);
			if (!elemVal) return nullptr;

			llvm::Value* dstPtr = builder.CreateInBoundsGEP(
					dyn->elemTy, data, { builder.getInt64(idx) }, "arr_to_dyn_dst");
			builder.CreateStore(elemVal, dstPtr);
		}

		llvm::Value* desc = llvm::PoisonValue::get(dynTy);
		desc = builder.CreateInsertValue(desc, data, 0);
		if (dyn->rank >= 1) {
			desc = builder.CreateInsertValue(desc, builder.getInt64(count), 1);
		}
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
		} else if (dynInfo(fieldTy)) {
			argVal = emitFixedArrayToDyn(node.arguments[i].get(), fieldTy);
			if (!argVal) return;
		} else {
			argVal = isUns
				? emitUnsignedValue(node.arguments[i].get(), fieldTy, "field '" + info.fieldNames[i] + "'")
				: (isRef ? emitRef(node.arguments[i].get()) : emit(node.arguments[i].get()));
			if (!argVal) return;

			if (fieldTy->isArrayTy() && argVal->getType()->isPointerTy()) {
				argVal = builder.CreateLoad(fieldTy, argVal, "arr_field_load");
			}

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
