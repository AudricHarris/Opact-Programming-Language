//==========================================================================//
//  LValues.cpp  -  addresses of assignable things                          //
//                                                                          //
//  emitLValue (variables, array elements, fields) and emitFieldAddr.       //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

llvm::Value* CodeGenerator::emitLValue(Expression* e) {
	if (!e) return nullptr;

	// 1. Identifier (e.g., `x`)
	if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return nullptr;
		}
		if (var->pointee || (var->structType && var->type->isPointerTy()))
			return builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_reborrow");
		return var->addr;
	}

	// 2. Array Index (e.g., `lstHuman[3]` or `plateau[row][col]` or `plateau[0]` or `dir[0]`)
	if (auto* aie = dynamic_cast<ArrayIndexExpr*>(e)) {
		if (!aie->array || aie->indices.empty()) return nullptr;
		llvm::Value* currentPtr = nullptr;
		llvm::Type* currentTy = nullptr;

		if (auto* id = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			VarInfo* var = lookupVar(id->name);
			if (!var) {
				error("Undefined variable '" + id->name + "'");
				return nullptr;
			}
			currentPtr = var->addr;
			currentTy = var->type;
			if (var->pointee) {
				currentPtr = builder.CreateLoad(builder.getPtrTy(), currentPtr, id->name + "_ref");
				currentTy = var->pointee;
			} else if (currentTy->isPointerTy()) {
				if (var->structType) {
					currentPtr = builder.CreateLoad(builder.getPtrTy(), currentPtr, id->name + "_ptr");
					currentTy = var->structType;
				} else {
					// Plain `ptr` (such as `str` from Read()) is an `i8` byte buffer, not a `%dynarray`.
					currentPtr = builder.CreateLoad(builder.getPtrTy(), currentPtr, id->name + "_ptr");
					currentTy = builder.getInt8Ty();
				}
			}
		} else if (auto* ma = dynamic_cast<MemberAccessExpr*>(aie->array.get())) {
			if (!emitFieldAddr(*ma, currentPtr, currentTy)) return nullptr;
		} else {
			currentPtr = emitRef(aie->array.get());
			currentTy = currentPtr ? currentPtr->getType() : nullptr;
		}

		if (!currentPtr || !currentTy) return nullptr;

		for (size_t i = 0; i < aie->indices.size(); ++i) {
			if (const DynArrayInfo* dyn = dynInfo(currentTy)) {
				size_t avail = aie->indices.size() - i;
				auto* dynStructTy = llvm::cast<llvm::StructType>(currentTy);
				llvm::Type* i64 = builder.getInt64Ty();

				if (avail >= dyn->rank) {
					std::vector<llvm::Value*> idxs;
					for (size_t k = 0; k < dyn->rank; ++k) {
						llvm::Value* v = emit(aie->indices[i + k].get());
						if (v) v = coerce(v, i64);
						if (!v) return nullptr;
						idxs.push_back(v);
					}
					currentPtr = dynElemPtr(builder, *dyn, dynStructTy, currentPtr, idxs);
					currentTy = dyn->elemTy;
					i += dyn->rank - 1;
					continue;
				}

				// Partial index (`avail < dyn->rank`): build a sub-descriptor of rank `dyn->rank - avail`.
				llvm::Value* idxVal = emit(aie->indices[i].get());
				if (!idxVal) return nullptr;
				idxVal = coerce(idxVal, i64);
				if (!idxVal) return nullptr;

				llvm::Value* dataPtr = builder.CreateLoad(
						builder.getPtrTy(),
						builder.CreateStructGEP(dynStructTy, currentPtr, 0),
						"dyn_sub_data");

				std::vector<llvm::Value*> dims;
				for (size_t k = 0; k < dyn->rank; ++k) {
					dims.push_back(builder.CreateLoad(
							i64,
							builder.CreateStructGEP(dynStructTy, currentPtr, static_cast<unsigned>(k + 1)),
							"dyn_sub_dim"));
				}

				llvm::Value* stride = dims[1];
				for (size_t k = 2; k < dyn->rank; ++k) {
					stride = builder.CreateMul(stride, dims[k], "dyn_sub_stride");
				}
				llvm::Value* offset = builder.CreateMul(idxVal, stride, "dyn_sub_off");
				llvm::Value* subData = builder.CreateInBoundsGEP(dyn->elemTy, dataPtr, { offset }, "dyn_sub_ptr");

				size_t remRank = dyn->rank - 1;
				llvm::StructType* subTy = getDynArrayType(context, dyn->elemTy, remRank);
				llvm::Value* desc = llvm::PoisonValue::get(subTy);
				desc = builder.CreateInsertValue(desc, subData, 0);
				for (size_t k = 1; k < dyn->rank; ++k) {
					desc = builder.CreateInsertValue(desc, dims[k], static_cast<unsigned>(k));
				}

				llvm::AllocaInst* subSlot = createEntryAlloca(currentFn, subTy, "dyn_sub_tmp");
				builder.CreateStore(desc, subSlot);
				currentPtr = subSlot;
				currentTy = subTy;
				continue;
			}

			llvm::Value* idx = emit(aie->indices[i].get());
			if (!idx) return nullptr;
			idx = coerce(idx, builder.getInt64Ty());

			if (currentTy->isArrayTy()) {
				llvm::ArrayType* arrTy = llvm::cast<llvm::ArrayType>(currentTy);
				currentPtr = builder.CreateInBoundsGEP(arrTy, currentPtr, { builder.getInt64(0), idx }, "array_idx_ptr");
				currentTy = arrTy->getElementType();
			} else {
				currentPtr = builder.CreateInBoundsGEP(builder.getInt8Ty(), currentPtr, { idx }, "ptr_idx_ptr");
				currentTy = builder.getInt8Ty();
			}
		}
		return currentPtr;
	}

	// 3. Member Access (e.g., `human.age` or `game.map[row].adjacentBombs`)
	if (auto* mae = dynamic_cast<MemberAccessExpr*>(e)) {
		llvm::Value* addr = nullptr;
		llvm::Type* ty = nullptr;
		if (emitFieldAddr(*mae, addr, ty)) {
			return addr;
		}
		return nullptr;
	}

	error("Cannot borrow non-lvalue expression");
	return nullptr;
}

// Computes the address of `obj.member` and the type stored there.
// If the field is a reference, the stored pointer is followed, so `addr`
// is the address of the referenced object and `type` is its type.
bool CodeGenerator::emitFieldAddr(MemberAccessExpr& ma, llvm::Value*& addr, llvm::Type*& type) {
	llvm::Value* objPtr = nullptr;
	llvm::StructType* structTy = nullptr;

	if (auto* objId = dynamic_cast<IdentifierExpr*>(ma.object.get())) {
		VarInfo* var = lookupVar(objId->name);
		if (!var) {
			error("Undefined variable '" + objId->name + "'");
			return false;
		}
		if (var->type->isPointerTy() && var->structType != nullptr) {
			objPtr   = builder.CreateLoad(builder.getPtrTy(), var->addr, objId->name + "_load");
			structTy = var->structType;
		} else {
			objPtr   = var->addr;
			structTy = var->structType ? var->structType : llvm::dyn_cast<llvm::StructType>(var->type);
		}
	}
	else if (auto* aie = dynamic_cast<ArrayIndexExpr*>(ma.object.get())) {
		objPtr = emitLValue(aie);

		llvm::Type* arrTy = nullptr;
		if (auto* baseId = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			if (VarInfo* var = lookupVar(baseId->name)) arrTy = var->type;
		} else if (auto* baseMa = dynamic_cast<MemberAccessExpr*>(aie->array.get())) {
			llvm::Value* tmpAddr = nullptr;
			llvm::Type* tmpTy = nullptr;
			if (emitFieldAddr(*baseMa, tmpAddr, tmpTy)) arrTy = tmpTy;
		}
		if (arrTy) arrTy = peelIndexTy(arrTy, aie->indices.size());
		if (arrTy) {
			structTy = llvm::dyn_cast<llvm::StructType>(arrTy);
		}
	}
	else {
		// Fallback for general expressions
		objPtr = emit(ma.object.get());
	}

	if (!structTy || !objPtr) {
		error("Member access '.' is only supported on structures");
		return false;
	}

	std::string structName = structTy->getName().str();
	auto it = structs.find(structName);
	if (it == structs.end()) {
		error("Unknown structure type metadata for '" + structName + "'");
		return false;
	}

	const StructInfo& info = it->second;
	auto fieldIt = std::find(info.fieldNames.begin(), info.fieldNames.end(), ma.memberName);
	if (fieldIt == info.fieldNames.end()) {
		error("Structure '" + structName + "' has no field named '" + ma.memberName + "'");
		return false;
	}
	size_t idx = static_cast<size_t>(std::distance(info.fieldNames.begin(), fieldIt));

	if (idx >= info.fieldIsRef.size() || idx >= info.fieldPointee.size()) {
		error("Internal error: incomplete metadata for structure '" + structName + "'");
		return false;
	}

	llvm::Value* fieldPtr = builder.CreateStructGEP(structTy, objPtr, idx, ma.memberName + "_ptr");
	if (info.fieldIsRef[idx] && info.fieldPointee[idx]) {
		addr = builder.CreateLoad(builder.getPtrTy(), fieldPtr, ma.memberName + "_ref");
		type = info.fieldPointee[idx];
	} else {
		addr = fieldPtr;
		type = structTy->getElementType(idx);
	}
	return true;
}
