//==========================================================================//
//  Arrays.cpp  -  array literals, allocation and indexing                  //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(ArrayLiteralExpr& node) {
	bool ctx = unsignedCtx;
	unsignedCtx = false;
	lastValue = nullptr;
	if (node.elements.empty()) {
		error("Empty array literal not supported without explicit type");
		return;
	}

	// Correctly resolve the expected element type (struct or nested array)
	llvm::Type* elemTy = getExprType(node.elements[0].get());
	if (!elemTy) {
		llvm::Value* firstVal = emit(node.elements[0].get());
		if (!firstVal) return;
		elemTy = firstVal->getType();
	}

	size_t numElements = node.elements.size();
	llvm::ArrayType* arrayTy = llvm::ArrayType::get(elemTy, numElements);
	if (!currentFn) {
		error("Array literal outside of a function");
		return;
	}

	llvm::AllocaInst* arrayAlloca = createEntryAlloca(currentFn, arrayTy, "array_lit");

	for (size_t i = 0; i < numElements; ++i) {
		llvm::Value* val = nullptr;
		Expression* el = node.elements[i].get();
		if (ctx) {
			if (dynamic_cast<ArrayLiteralExpr*>(el)) { unsignedCtx = true; val = emit(el); }
			else val = emitUnsignedValue(el, elemTy, "array element");
		} else {
			val = emit(el);
		}
		if (!val) return;

		// If the element type is a struct or array, load it from its temporary pointer
		if ((elemTy->isStructTy() || elemTy->isArrayTy()) && val->getType()->isPointerTy()) {
			val = builder.CreateLoad(elemTy, val, "elem_load");
		} else {
			val = coerce(val, elemTy);
		}

		llvm::Value* elemPtr = builder.CreateInBoundsGEP(
				arrayTy,
				arrayAlloca,
				{ builder.getInt64(0), builder.getInt64(i) },
				"array_elem_ptr"
				);
		builder.CreateStore(val, elemPtr);
	}

	lastValue = arrayAlloca;
}

// `Tile[20][20] name` on its own: a zero-initialised fixed-size array (on the
// stack). Inside a structure literal whose field is a '?' array, see
// visit(StructCreateExpr): the same syntax allocates heap storage instead.
void CodeGenerator::visit(ArrayAllocExpr& node) {
	lastValue = nullptr;
	if (!currentFn) { error("Array allocation outside of a function"); return; }
	if (hasUnknownDim(node.type)) {
		error("Cannot allocate an array with an unknown ('?') dimension: give every dimension a size");
		return;
	}
	if (node.type.dim.empty()) {
		error("Array allocation needs at least one dimension, e.g. Tile[20][20]");
		return;
	}
	llvm::Type* ty = mapType(node.type);
	if (!ty || ty->isVoidTy()) { error("Invalid element type in array allocation"); return; }

	llvm::AllocaInst* slot = createEntryAlloca(currentFn, ty, node.name.empty() ? "array_alloc" : node.name);
	builder.CreateStore(llvm::Constant::getNullValue(ty), slot);

	if (node.initExpr && *node.initExpr) {
		bool prevUnsigned = unsignedCtx;
		unsignedCtx = node.type.isUnsignedInteger();
		llvm::Value* initVal = emit(node.initExpr->get());
		unsignedCtx = prevUnsigned;

		if (!initVal) return;

		if (ty->isArrayTy() && initVal->getType()->isPointerTy()) {
			llvm::Value* loaded = builder.CreateLoad(ty, initVal, "arr_init_load");
			builder.CreateStore(loaded, slot);
		} else {
			llvm::Value* coerced = coerce(initVal, ty);
			if (!coerced) return;
			builder.CreateStore(coerced, slot);
		}
	}

	lastValue = slot;
}

void CodeGenerator::visit(ArrayIndexExpr& node) {
	lastValue = nullptr;
	if (!node.array || node.indices.empty()) return;

	llvm::Value* currentPtr = nullptr;
	llvm::Type* currentTy = nullptr;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.array.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
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
			} else if (!g_dynArrays.empty() && id->name != "dir") {
				currentPtr = builder.CreateLoad(builder.getPtrTy(), currentPtr, id->name + "_ptr");
				currentTy = const_cast<llvm::Type*>(g_dynArrays.begin()->first);
			} else {
				currentPtr = builder.CreateLoad(builder.getPtrTy(), currentPtr, id->name + "_ptr");
				currentTy = builder.getInt8Ty();
			}
		}
	} else if (auto* ma = dynamic_cast<MemberAccessExpr*>(node.array.get())) {
		llvm::Value* fieldAddr = nullptr;
		llvm::Type* fieldTy = nullptr;
		if (!emitFieldAddr(*ma, fieldAddr, fieldTy)) {
			return;
		}
		currentPtr = fieldAddr;
		currentTy = fieldTy;
	} else {
		currentPtr = emitRef(node.array.get());
		currentTy = getExprType(node.array.get());
	}

	if (!currentPtr || !currentTy) {
		error("Invalid array expression");
		return;
	}

	for (size_t i = 0; i < node.indices.size(); ++i) {
		if (const DynArrayInfo* dyn = dynInfo(currentTy)) {
			size_t avail = node.indices.size() - i;
			auto* dynStructTy = llvm::cast<llvm::StructType>(currentTy);
			llvm::Type* i64 = builder.getInt64Ty();

			if (avail >= dyn->rank) {
				std::vector<llvm::Value*> idxs;
				for (size_t k = 0; k < dyn->rank; ++k) {
					llvm::Value* v = emit(node.indices[i + k].get());
					if (v) v = coerce(v, i64);
					if (!v) return;
					idxs.push_back(v);
				}
				currentPtr = dynElemPtr(builder, *dyn, dynStructTy, currentPtr, idxs);
				currentTy = dyn->elemTy;
				i += dyn->rank - 1;
				continue;
			}

			llvm::Value* idxVal = emit(node.indices[i].get());
			if (!idxVal) return;
			idxVal = coerce(idxVal, i64);
			if (!idxVal) return;

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

		llvm::Value* idx = emit(node.indices[i].get());
		if (!idx) return;
		idx = coerce(idx, builder.getInt64Ty());

		if (currentTy->isArrayTy()) {
			llvm::ArrayType* arrTy = llvm::cast<llvm::ArrayType>(currentTy);
			currentPtr = builder.CreateInBoundsGEP(arrTy, currentPtr, { builder.getInt64(0), idx }, "array_idx");
			currentTy = arrTy->getElementType();
		} else if (currentPtr->getType()->isPointerTy()) {
			currentPtr = builder.CreateInBoundsGEP(builder.getInt8Ty(), currentPtr, { idx }, "ptr_idx");
			currentTy = builder.getInt8Ty();
		} else {
			error("Indexing applied to non-array type");
			return;
		}
	}

	if (currentTy->isArrayTy() || dynInfo(currentTy)) {
		lastValue = currentPtr;
	} else {
		lastValue = builder.CreateLoad(currentTy, currentPtr, "val_load");
	}
}
