//==========================================================================//
//  Types.cpp  -  type system                                               //
//                                                                          //
//  mapType (language type -> LLVM type), coerce, unify, toBool, getExprType.//
//  Single place to extend when adding a new primitive type.                //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

llvm::Type* CodeGenerator::mapType(const TypeDesc& d) {
	if (d.isReference) {
		return builder.getPtrTy();
	}
	llvm::Type* base = nullptr;
	switch (d.type) {
		case DataType::I8:  case DataType::U8:  base = builder.getInt8Ty();   break;
		case DataType::I16: case DataType::U16: base = builder.getInt16Ty();  break;
		case DataType::I32: case DataType::Int:  base = builder.getInt32Ty();   break;
		case DataType::U32: case DataType::UInt: base = builder.getInt32Ty();  break;
		case DataType::I64: case DataType::U64: base = builder.getInt64Ty();  break;
		case DataType::F32: base = builder.getFloatTy();   break;
		case DataType::F64: case DataType::Float: base = builder.getDoubleTy(); break;
		case DataType::Bool:  base = builder.getInt1Ty();    break;
		case DataType::Char:  base = builder.getInt8Ty();    break;
		case DataType::Str:   base = builder.getPtrTy();     break;
		case DataType::Void:  base = builder.getVoidTy();    break;
		case DataType::Custom: {
			auto it = structs.find(d.customTypeName);
			if (it != structs.end()) { base = it->second.type; break; }
			error("Unknown type '" + d.customTypeName + "'");
			return nullptr;
		}
	}
	if (!base) return nullptr;

	// Any '?' dimension -> heap-backed descriptor for the whole array.
	if (hasUnknownDim(d)) {
		if (base->isVoidTy()) return nullptr;
		return getDynArrayType(context, base, d.dim.size());
	}

	for (auto it = d.dim.rbegin(); it != d.dim.rend(); ++it) {
		base = llvm::ArrayType::get(base, static_cast<uint64_t>(*it));
	}
	return base;
}

// For a reference type, the type the pointer points at (nullptr otherwise).
llvm::Type* CodeGenerator::pointeeOf(const TypeDesc& d) {
	if (!d.isReference) return nullptr;
	TypeDesc inner = d;
	inner.isReference = false;
	llvm::Type* p = mapType(inner);
	return (p && !p->isVoidTy()) ? p : nullptr;
}

// Anything -> i1 (non-zero / non-null is true).
llvm::Value* CodeGenerator::toBool(llvm::Value* v) {
	if (!v) return nullptr;
	llvm::Type* t = v->getType();
	if (t->isIntegerTy(1)) return v;
	if (t->isFloatingPointTy())
		return builder.CreateFCmpUNE(v, llvm::ConstantFP::get(t, 0.0), "tobool");
	return builder.CreateIsNotNull(v, "tobool"); // ints and pointers
}

llvm::Value* CodeGenerator::coerce(llvm::Value* v, llvm::Type* to) {
	if (!v || !to) return nullptr;
	llvm::Type* from = v->getType();
	if (from == to) return v;

	if (from->isPointerTy() && to->isPointerTy())
		return builder.CreatePointerCast(v, to, "ptrcast");

	if (to->isIntegerTy(1)) return toBool(v);

	if (from->isIntegerTy() && to->isIntegerTy()) {
		unsigned fb = from->getIntegerBitWidth(), tb = to->getIntegerBitWidth();
		if (tb < fb) return builder.CreateTrunc(v, to, "trunc");
		if (fb == 1) return builder.CreateZExt(v, to, "zext");

		return builder.CreateSExt(v, to, "sext");
	}
	if (from->isIntegerTy() && to->isFloatingPointTy())
		return from->isIntegerTy(1) ? builder.CreateUIToFP(v, to, "uitofp")
			: builder.CreateSIToFP(v, to, "sitofp");
	if (from->isFloatingPointTy() && to->isIntegerTy())
		return builder.CreateFPToSI(v, to, "fptosi");
	if (from->isFloatingPointTy() && to->isFloatingPointTy())
		return from->getPrimitiveSizeInBits() < to->getPrimitiveSizeInBits()
			? builder.CreateFPExt(v, to, "fpext") : builder.CreateFPTrunc(v, to, "fptrunc");

	if (from->isPointerTy() && to->isStructTy()) {
		return builder.CreateLoad(to, v, "struct_coerce_load");
	}

	if (from->isPointerTy() && to->isIntegerTy()) {
		if (to->isIntegerTy(1)) return toBool(v);
		if (!currentFn) {
			error("String-to-int conversion outside of a function");
			return nullptr;
		}
		auto atoiFn = module->getOrInsertFunction(
				"atoi", llvm::FunctionType::get(builder.getInt32Ty(), { builder.getPtrTy() }, false));
		llvm::Value* strPtr = builder.CreatePointerCast(v, builder.getPtrTy(), "atoi_str");
		llvm::Value* parsed = builder.CreateCall(atoiFn, { strPtr }, "atoi_call");
		if (parsed->getType() != to) {
			parsed = (to->getIntegerBitWidth() < 32)
				? builder.CreateTrunc(parsed, to, "atoi_trunc")
				: builder.CreateSExt(parsed, to, "atoi_sext");
		}
		return parsed;
	}

	if (from->isIntegerTy() && to->isPointerTy()) {
		if (!currentFn) {
			error("Int-to-string conversion outside of a function");
			return nullptr;
		}

		// Single char (i8) -> 2-byte heap string so `char as str` produces a C string.
		if (from->isIntegerTy(8)) {
			auto mallocFn = module->getOrInsertFunction(
				"malloc",
				llvm::FunctionType::get(builder.getPtrTy(), { builder.getInt64Ty() }, false)
			);
			llvm::Value* buf = builder.CreateCall(
				mallocFn,
				{ llvm::ConstantInt::get(builder.getInt64Ty(), 2) },
				"char_str_buf"
			);
			llvm::AllocaInst* slot = createHeapSlot("heap_char_str");
			builder.CreateStore(buf, slot);
			registerCleanup(slot);
			lastHeapSlot = slot;

			builder.CreateStore(v, buf);
			llvm::Value* nullTermPtr = builder.CreateInBoundsGEP(
				builder.getInt8Ty(), buf, { builder.getInt64(1) }, "char_str_null_ptr"
			);
			builder.CreateStore(llvm::ConstantInt::get(builder.getInt8Ty(), 0), nullTermPtr);
			return builder.CreatePointerCast(buf, to, "char_to_str");
		}

		// Normalize integer to i64 for "%lld"
		llvm::Value* val64 = v;
		if (val64->getType()->getIntegerBitWidth() < 64) {
			val64 = (val64->getType()->getIntegerBitWidth() == 1)
				? builder.CreateZExt(val64, builder.getInt64Ty(), "i1_to_i64")
				: builder.CreateSExt(val64, builder.getInt64Ty(), "int_to_i64");
		} else if (val64->getType()->getIntegerBitWidth() > 64) {
			val64 = builder.CreateTrunc(val64, builder.getInt64Ty(), "int_to_i64_trunc");
		}

		// Allocate 32 bytes on heap so it survives returning from functions like Color()
		const uint64_t bufSize = 32;
		auto mallocFn = module->getOrInsertFunction(
			"malloc",
			llvm::FunctionType::get(builder.getPtrTy(), { builder.getInt64Ty() }, false)
		);
		llvm::Value* buf = builder.CreateCall(
			mallocFn,
			{ llvm::ConstantInt::get(builder.getInt64Ty(), bufSize) },
			"itoa_buf"
		);

		// int snprintf(char *str, size_t size, const char *format, ...)
		auto snprintfFn = module->getOrInsertFunction(
			"snprintf",
			llvm::FunctionType::get(
				builder.getInt32Ty(),
				{ builder.getPtrTy(), builder.getInt64Ty(), builder.getPtrTy() },
				true
			)
		);
		llvm::Value* fmt = builder.CreateGlobalString("%lld", "fmt_lld");
		builder.CreateCall(
			snprintfFn,
			{ buf, llvm::ConstantInt::get(builder.getInt64Ty(), bufSize), fmt, val64 },
			"snprintf_call"
		);

		return builder.CreatePointerCast(buf, to, "itoa_cast");
	}

	std::string fromStr;
	std::string toStr;
	llvm::raw_string_ostream rsoFrom(fromStr);
	llvm::raw_string_ostream rsoTo(toStr);
	from->print(rsoFrom);
	to->print(rsoTo);
	error("Cannot convert between these types: from " + rsoFrom.str() + " to " + rsoTo.str());
	return nullptr;
}

// C variadic promotion: small ints -> i32 (floats are already double).
llvm::Value* CodeGenerator::promoteVarArg(llvm::Value* v) {
	llvm::Type* t = v->getType();
	if (t->isIntegerTy() && t->getIntegerBitWidth() < 32) return coerce(v, builder.getInt32Ty());
	if (t->isFloatTy()) return builder.CreateFPExt(v, builder.getDoubleTy(), "fpext");
	return v;
}

// Brings both operands to a common numeric type.
bool CodeGenerator::unify(llvm::Value*& l, llvm::Value*& r) {
	llvm::Type* lt = l->getType();
	llvm::Type* rt = r->getType();
	if (lt == rt) return true;

	if (lt->isFloatingPointTy() || rt->isFloatingPointTy()) {
		llvm::Type* d = builder.getDoubleTy();
		l = coerce(l, d);
		r = coerce(r, d);
		return l && r;
	}
	if (lt->isIntegerTy() && rt->isIntegerTy()) {
		llvm::Type* wide = lt->getIntegerBitWidth() >= rt->getIntegerBitWidth() ? lt : rt;
		l = coerce(l, wide);
		r = coerce(r, wide);
		return l && r;
	}

	if (lt->isPointerTy() && !rt->isPointerTy()) {
		l = builder.CreateLoad(rt, l, "unref_l");
		lt = l->getType();
	} else if (rt->isPointerTy() && !lt->isPointerTy()) {
		r = builder.CreateLoad(lt, r, "unref_r");
		rt = r->getType();
	}
	if (lt == rt) return true;

	error("Operand types do not match");
	return false;
}

llvm::Type* CodeGenerator::getExprType(Expression* e) {
	if (!e) return nullptr;
	if (auto* sce = dynamic_cast<StructCreateExpr*>(e)) {
		if (auto* id = dynamic_cast<IdentifierExpr*>(sce->structObj.get())) {
			auto it = structs.find(id->name);
			if (it != structs.end()) return it->second.type;
		}
	}
	if (auto* aae = dynamic_cast<ArrayAllocExpr*>(e)) {
		if (!hasUnknownDim(aae->type)) return mapType(aae->type);
	}
	if (auto* ale = dynamic_cast<ArrayLiteralExpr*>(e)) {
		if (!ale->elements.empty()) {
			llvm::Type* innerElemTy = getExprType(ale->elements[0].get());
			if (innerElemTy) {
				return llvm::ArrayType::get(innerElemTy, ale->elements.size());
			}
		}
	}
	llvm::Value* v = emit(e);
	return v ? v->getType() : nullptr;
}
