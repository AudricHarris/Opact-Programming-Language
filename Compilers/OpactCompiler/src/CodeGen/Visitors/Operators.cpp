//==========================================================================//
//  Operators.cpp  -  binary / unary / postfix / cast                       //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

llvm::Value* CodeGenerator::emitBinary(BinaryOp op, llvm::Value* l, llvm::Value* r, bool isUnsigned) {
	if (op == BinaryOp::And) return builder.CreateAnd(toBool(l), toBool(r), "andtmp");
	if (op == BinaryOp::Or)  return builder.CreateOr(toBool(l),  toBool(r), "ortmp");

	llvm::Type* lt = l->getType();
	llvm::Type* rt = r->getType();

	auto charToStrPtr = [&](llvm::Value* chVal) -> llvm::Value* {
		llvm::Value* ch8 = coerce(chVal, builder.getInt8Ty());
		if (!ch8) return nullptr;
		llvm::ArrayType* bufTy = llvm::ArrayType::get(builder.getInt8Ty(), 2);
		llvm::AllocaInst* buf = createEntryAlloca(currentFn, bufTy, "ch_str_buf");
		builder.CreateStore(ch8, builder.CreateInBoundsGEP(bufTy, buf, { builder.getInt64(0), builder.getInt64(0) }));
		builder.CreateStore(llvm::ConstantInt::get(builder.getInt8Ty(), 0),
				builder.CreateInBoundsGEP(bufTy, buf, { builder.getInt64(0), builder.getInt64(1) }));
		return builder.CreateInBoundsGEP(bufTy, buf, { builder.getInt64(0), builder.getInt64(0) }, "ch_str");
	};

	// String + char / char + string concatenation
	if (op == BinaryOp::Add &&
			((lt->isPointerTy() && rt->isIntegerTy(8)) || (lt->isIntegerTy(8) && rt->isPointerTy()))) {
		if (!currentFn) {
			error("String concatenation outside of a function");
			return nullptr;
		}
		if (lt->isIntegerTy(8)) {
			l = charToStrPtr(l);
			if (!l) return nullptr;
		} else {
			r = charToStrPtr(r);
			if (!r) return nullptr;
		}
		lt = l->getType();
		rt = r->getType();
	}

	// String/pointer concatenation BEFORE unify so different pointer types don't trigger errors
	if (op == BinaryOp::Add && !lt->isFloatingPointTy() && !lt->isIntegerTy() &&
			!rt->isFloatingPointTy() && !rt->isIntegerTy()) {
		if (!currentFn) {
			error("String concatenation outside of a function");
			return nullptr;
		}
		auto charPtrTy = builder.getPtrTy();
		auto sizeTy = builder.getInt64Ty();
		auto strlenFn = module->getOrInsertFunction("strlen", llvm::FunctionType::get(sizeTy, { charPtrTy }, false));
		auto mallocFn = module->getOrInsertFunction("malloc", llvm::FunctionType::get(charPtrTy, { sizeTy }, false));
		auto memcpyFn = module->getOrInsertFunction("memcpy", llvm::FunctionType::get(charPtrTy, { charPtrTy, charPtrTy, sizeTy }, false));
		llvm::Value* str1 = builder.CreatePointerCast(l, charPtrTy, "str1");
		llvm::Value* str2 = builder.CreatePointerCast(r, charPtrTy, "str2");
		llvm::Value* len1 = builder.CreateCall(strlenFn, { str1 }, "len1");
		llvm::Value* len2 = builder.CreateCall(strlenFn, { str2 }, "len2");
		llvm::Value* totalLen = builder.CreateAdd(
				builder.CreateAdd(len1, len2, "addlen"),
				llvm::ConstantInt::get(sizeTy, 1),
				"totallen");
		llvm::Value* dest = builder.CreateCall(mallocFn, { totalLen }, "dest");

		// Own the heap string through a null-initialised entry-block slot.
		llvm::AllocaInst* slot = createHeapSlot("heap_str");
		builder.CreateStore(dest, slot);
		registerCleanup(slot);
		lastHeapSlot = slot;

		builder.CreateCall(memcpyFn, { dest, str1, len1 });
		llvm::Value* destOffset = builder.CreateInBoundsGEP(builder.getInt8Ty(), dest, { len1 }, "dest_offset");
		llvm::Value* len2Plus1 = builder.CreateAdd(len2, llvm::ConstantInt::get(sizeTy, 1), "len2_plus_1");
		builder.CreateCall(memcpyFn, { destOffset, str2, len2Plus1 });
		return dest;
	}

	if (!unify(l, r)) return nullptr;
	lt = l->getType();
	rt = r->getType();
	bool fp = lt->isFloatingPointTy();

	switch (op) {
		case BinaryOp::Add: return fp ? builder.CreateFAdd(l, r, "addtmp") : builder.CreateAdd(l, r, "addtmp");
		case BinaryOp::Sub: return fp ? builder.CreateFSub(l, r, "subtmp") : builder.CreateSub(l, r, "subtmp");
		case BinaryOp::Mul: return fp ? builder.CreateFMul(l, r, "multmp") : builder.CreateMul(l, r, "multmp");
		case BinaryOp::Div:
			return fp ? builder.CreateFDiv(l, r, "divtmp")
				: (isUnsigned ? builder.CreateUDiv(l, r, "udivtmp") : builder.CreateSDiv(l, r, "sdivtmp"));
		case BinaryOp::Mod:
			return fp ? builder.CreateFRem(l, r, "modtmp")
				: (isUnsigned ? builder.CreateURem(l, r, "uremtmp") : builder.CreateSRem(l, r, "sremtmp"));

		case BinaryOp::Eq:
			if (lt->isPointerTy() && rt->isPointerTy()) {
				auto charPtrTy = builder.getPtrTy();
				auto i32Ty = builder.getInt32Ty();
				auto strcmpFn = module->getOrInsertFunction(
						"strcmp", llvm::FunctionType::get(i32Ty, { charPtrTy, charPtrTy }, false));
				llvm::Value* cmpRes = builder.CreateCall(
						strcmpFn,
						{ builder.CreatePointerCast(l, charPtrTy, "strcmp_s1"),
						  builder.CreatePointerCast(r, charPtrTy, "strcmp_s2") },
						"strcmp_res");
				return builder.CreateICmpEQ(cmpRes, llvm::ConstantInt::get(i32Ty, 0), "streqtmp");
			}
			return fp ? builder.CreateFCmpOEQ(l, r, "cmptmp") : builder.CreateICmpEQ(l, r, "cmptmp");

		case BinaryOp::Ne:
			if (lt->isPointerTy() && rt->isPointerTy()) {
				auto charPtrTy = builder.getPtrTy();
				auto i32Ty = builder.getInt32Ty();
				auto strcmpFn = module->getOrInsertFunction(
						"strcmp", llvm::FunctionType::get(i32Ty, { charPtrTy, charPtrTy }, false));
				llvm::Value* cmpRes = builder.CreateCall(
						strcmpFn,
						{ builder.CreatePointerCast(l, charPtrTy, "strcmp_s1"),
						  builder.CreatePointerCast(r, charPtrTy, "strcmp_s2") },
						"strcmp_res");
				return builder.CreateICmpNE(cmpRes, llvm::ConstantInt::get(i32Ty, 0), "strnetmp");
			}
			return fp ? builder.CreateFCmpUNE(l, r, "cmptmp") : builder.CreateICmpNE(l, r, "cmptmp");
		case BinaryOp::Lt: return fp ? builder.CreateFCmpOLT(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpULT(l, r, "cmptmp") : builder.CreateICmpSLT(l, r, "cmptmp"));
		case BinaryOp::Gt: return fp ? builder.CreateFCmpOGT(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpUGT(l, r, "cmptmp") : builder.CreateICmpSGT(l, r, "cmptmp"));
		case BinaryOp::Le: return fp ? builder.CreateFCmpOLE(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpULE(l, r, "cmptmp") : builder.CreateICmpSLE(l, r, "cmptmp"));
		case BinaryOp::Ge: return fp ? builder.CreateFCmpOGE(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpUGE(l, r, "cmptmp") : builder.CreateICmpSGE(l, r, "cmptmp"));
		default: break;
	}
	error("Unsupported binary operator");
	return nullptr;
}

void CodeGenerator::visit(PostfixExpr& node) {
	lastValue = nullptr;
	llvm::Value* targetAddr = nullptr;
	llvm::Type* targetType = nullptr;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.expr.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		if (var->pointee) {
			targetAddr = builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_ref");
			targetType = var->pointee;
		} else {
			targetAddr = var->addr;
			targetType = var->type;
		}
	}
	else if (auto* ma = dynamic_cast<MemberAccessExpr*>(node.expr.get())) {
		if (!emitFieldAddr(*ma, targetAddr, targetType)) return;
	}
	else {
		error("++/-- requires a variable identifier or struct field");
		return;
	}

	llvm::Value* old = builder.CreateLoad(targetType, targetAddr, "old");
	bool inc = node.op == PostfixOp::Increment;
	llvm::Value* updated;

	if (targetType->isFloatingPointTy()) {
		llvm::Value* one = llvm::ConstantFP::get(targetType, 1.0);
		updated = inc ? builder.CreateFAdd(old, one, "inc") : builder.CreateFSub(old, one, "dec");
	} else if (targetType->isIntegerTy()) {
		llvm::Value* one = llvm::ConstantInt::get(targetType, 1);
		updated = inc ? builder.CreateAdd(old, one, "inc") : builder.CreateSub(old, one, "dec");
	} else {
		error("++/-- requires a numeric variable or reference");
		return;
	}

	builder.CreateStore(updated, targetAddr);
	lastValue = old; // postfix yields the previous value
}

void CodeGenerator::visit(BinaryExpr& node) {
	llvm::Value* l = emit(node.left.get());
	llvm::Value* r = emit(node.right.get());
	bool isUnsigned = isUnsignedExpr(node.left.get()) || isUnsignedExpr(node.right.get());

	if ((node.op == BinaryOp::Eq || node.op == BinaryOp::Ne) && l && r) {
		if (findEnumStorageType(l->getType()) || findEnumStorageType(r->getType())) {
			lastValue = emitEnumComparison(node.op, l, r);
			return;
		}
	}

	lastValue = (l && r) ? emitBinary(node.op, l, r, isUnsigned) : nullptr;
}

void CodeGenerator::visit(UnaryExpr& node) {
	// `-x` makes no sense for an unsigned variable.
	if (node.op == UnaryOp::Negate && isUnsignedExpr(node.expr.get())) {
		error("Cannot negate an unsigned value");
		lastValue = nullptr;
		return;
	}

	llvm::Value* v = emit(node.expr.get());
	if (!v) { lastValue = nullptr; return; }

	if (node.op == UnaryOp::Negate) {
		lastValue = v->getType()->isFloatingPointTy() ? builder.CreateFNeg(v, "negtmp")
			: builder.CreateNeg(v, "negtmp");
	} else {
		lastValue = builder.CreateNot(toBool(v), "nottmp");
	}
}

void CodeGenerator::visit(CastExpr& node) {
	llvm::Value* v = emit(node.expr.get());
	llvm::Type* to = mapType(node.castType);
	lastValue = (v && to && !to->isVoidTy()) ? coerce(v, to) : nullptr;
}
