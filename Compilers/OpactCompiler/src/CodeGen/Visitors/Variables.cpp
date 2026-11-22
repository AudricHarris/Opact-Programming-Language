//==========================================================================//
//  Variables.cpp  -  declarations and assignment of variables              //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(VarDeclExpr& node) {
	std::string name = tokenText(node.name);
	if (!currentFn) { error("Variable '" + name + "' declared outside of a function"); return; }

	bool isUnsigned = node.type.isUnsignedInteger();

	llvm::Value* init = nullptr;
	if (node.expr) {
		if (node.type.isReference) {
			init = emitRef(node.expr.get());
		} else if (isUnsigned) {
			llvm::Type* want = mapType(node.type);
			if (want && want->isIntegerTy()) {
				init = emitUnsignedValue(node.expr.get(), want, "variable '" + name + "'");
			} else {
				unsignedCtx = dynamic_cast<ArrayLiteralExpr*>(node.expr.get()) != nullptr;
				init = emit(node.expr.get());
				unsignedCtx = false;
			}
		} else {
			init = emit(node.expr.get());
		}
	}
	if (node.expr && !init) { lastValue = nullptr; return; }

	llvm::Type* ty = (node.type.type == DataType::Void && init) ? init->getType() : mapType(node.type);
	if (!ty || ty->isVoidTy()) { error("Invalid type for variable '" + name + "'"); return; }


	llvm::Value* initVal = nullptr;
	if (init) {
		if ((ty->isStructTy() || ty->isArrayTy()) && init->getType()->isPointerTy()) {
			initVal = builder.CreateLoad(ty, init, "init_load");
		} else {
			initVal = coerce(init, ty);
		}
	} else {
		initVal = llvm::Constant::getNullValue(ty);
	}
	if (!initVal) return;

	llvm::AllocaInst* slot = createEntryAlloca(currentFn, ty, name);
	builder.CreateStore(initVal, slot);

	llvm::StructType* st = nullptr;
	if (node.type.type == DataType::Custom) {
		auto it = structs.find(node.type.customTypeName);
		if (it != structs.end()) st = it->second.type;
	}

	// For references to non-struct types remember what the pointer points at.
	llvm::Type* pointee = pointeeOf(node.type);
	if (pointee && pointee->isStructTy()) pointee = nullptr;

	scopes.back()[name] = { slot, ty, st, pointee, isUnsigned };
	lastValue = initVal;
}

void CodeGenerator::visit(AssignExpr& node) {
	lastValue = nullptr;
	llvm::Value* targetPtr = nullptr;
	llvm::Type* targetType = nullptr;
	bool targetUnsigned = false;
	VarInfo* targetVar = nullptr;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.target.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		targetVar = var;
		targetUnsigned = var->isUnsigned;
		if (var->pointee) {
			// `r = value` on a reference writes to the referenced object.
			targetPtr  = builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_ref");
			targetType = var->pointee;
		} else {
			targetPtr  = var->addr;
			targetType = var->type;
		}
	}
	else if (auto* ma = dynamic_cast<MemberAccessExpr*>(node.target.get())) {
		if (!emitFieldAddr(*ma, targetPtr, targetType)) return;
		targetUnsigned = isUnsignedExpr(ma);
	}
	else if (auto* aie = dynamic_cast<ArrayIndexExpr*>(node.target.get())) {
		targetPtr = emitLValue(aie);
		if (!targetPtr) return;

		llvm::Type* elemTy = nullptr;
		if (auto* baseId = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			if (VarInfo* var = lookupVar(baseId->name)) elemTy = var->type;
		} else if (auto* baseMa = dynamic_cast<MemberAccessExpr*>(aie->array.get())) {
			llvm::Value* tmpAddr = nullptr;
			if (!emitFieldAddr(*baseMa, tmpAddr, elemTy)) return;
		}
		if (elemTy) elemTy = peelIndexTy(elemTy, aie->indices.size());
		targetType = elemTy;
		targetUnsigned = isUnsignedExpr(aie);
	}
	else {
		error("Only variables or structure fields can be assigned to");
		return;
	}

	if (!targetType) {
		error("Could not resolve target type for assignment");
		return;
	}

	lastHeapSlot = nullptr;
	llvm::Value* raw = (targetUnsigned && targetType->isIntegerTy())
		? emitUnsignedValue(node.value.get(), targetType, "variable")
		: emit(node.value.get());
	llvm::Value* v = coerce(raw, targetType);
	if (!v) {
		lastHeapSlot = nullptr;
		lastValue = nullptr;
		return;
	}

	builder.CreateStore(v, targetPtr);

	// Transfer heap ownership right after storing into `targetPtr`
	llvm::Value* slot = lastHeapSlot;
	lastHeapSlot = nullptr;
	if (slot) {
		transferHeapSlot(targetVar, slot);
	}

	lastValue = v;
}

void CodeGenerator::visit(BorrowExpr& node) {
	lastValue = emitLValue(node.expr.get());
}
