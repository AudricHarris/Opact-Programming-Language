//==========================================================================//
//  Unsigned.cpp  -  unsigned integers and constant evaluation              //
//                                                                          //
//  Literal parsing, compile-time integer evaluation, negative-value checks.//
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

bool CodeGenerator::isUnsignedExpr(Expression* e) {
	if (!e) return false;

	if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
		VarInfo* var = lookupVar(id->name);
		if (var) return var->isUnsigned;
		return false;
	}

	if (auto* castExpr = dynamic_cast<CastExpr*>(e)) {
		DataType dt = castExpr->castType.type;
		return dt == DataType::U8 || dt == DataType::U16 ||
			dt == DataType::U32 || dt == DataType::U64 || dt == DataType::UInt;
	}

	if (auto* binExpr = dynamic_cast<BinaryExpr*>(e)) {
		return isUnsignedExpr(binExpr->left.get()) || isUnsignedExpr(binExpr->right.get());
	}

	if (auto* pf = dynamic_cast<PostfixExpr*>(e)) return isUnsignedExpr(pf->expr.get());

	if (auto* aie = dynamic_cast<ArrayIndexExpr*>(e)) {
		if (auto* base = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			if (VarInfo* var = lookupVar(base->name)) return var->isUnsigned;
		}
		return false;
	}

	if (auto* call = dynamic_cast<CallExpr*>(e)) {
		if (auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
			auto it = fnSigns.find(id->name == "Main" ? "main" : id->name);
			if (it != fnSigns.end()) return it->second.retUnsigned;
		}
		return false;
	}

	if (auto* ma = dynamic_cast<MemberAccessExpr*>(e)) {
		Expression* obj = ma->object.get();
		while (auto* aie = dynamic_cast<ArrayIndexExpr*>(obj)) obj = aie->array.get();

		if (auto* id = dynamic_cast<IdentifierExpr*>(obj)) {
			if (VarInfo* var = lookupVar(id->name)) {
				llvm::Type* t = var->type;
				while (t->isArrayTy()) t = llvm::cast<llvm::ArrayType>(t)->getElementType();
				llvm::StructType* st = var->structType ? var->structType : llvm::dyn_cast<llvm::StructType>(t);
				if (st) {
					auto it = structs.find(st->getName().str());
					if (it != structs.end()) {
						const auto& names = it->second.fieldNames;
						auto f = std::find(names.begin(), names.end(), ma->memberName);
						size_t idx = static_cast<size_t>(f - names.begin());
						if (f != names.end() && idx < it->second.fieldIsUnsigned.size())
							return it->second.fieldIsUnsigned[idx];
					}
				}
			}
		}
		return false;
	}

	return false;
}

// Parses an integer literal into sign + magnitude. Handles '_' separators,
// a leading '-', and 0x / 0o / 0b prefixes.
bool CodeGenerator::parseIntLiteral(LiteralExpr& lit, bool& neg, unsigned long long& mag) {
	std::string text = tokenText(lit.lit);
	text.erase(std::remove(text.begin(), text.end(), '_'), text.end());

	neg = false;
	size_t skip = 0;
	if (!text.empty() && text[0] == '-') { neg = true; skip = 1; }

	int base = 10;
	switch (lit.base) {
		case NumericBase::Hexadecimal: base = 16; skip += 2; break;
		case NumericBase::Octal:       base = 8;  skip += 2; break;
		case NumericBase::Binary:      base = 2;  skip += 2; break;
		default: break;
	}
	try {
		mag = std::stoull(text.substr(skip), nullptr, base);
	} catch (...) {
		error("Invalid integer literal '" + text + "'");
		return false;
	}
	if (mag == 0) neg = false; // -0 is just 0
	return true;
}

// Compile-time value of an integer constant expression: `5`, `-5`, `-(-5)`.
// Returns false if `e` is not such a constant.
bool CodeGenerator::evalConstInt(Expression* e, bool& neg, unsigned long long& mag) {
	if (auto* lit = dynamic_cast<LiteralExpr*>(e)) {
		if (lit->kind != LiteralKind::Int) return false;
		return parseIntLiteral(*lit, neg, mag);
	}
	if (auto* un = dynamic_cast<UnaryExpr*>(e); un && un->op == UnaryOp::Negate) {
		if (!evalConstInt(un->expr.get(), neg, mag)) return false;
		if (mag != 0) neg = !neg;
		return true;
	}
	return false;
}

llvm::Value* CodeGenerator::emitUnsignedValue(Expression* e, llvm::Type* ty, const std::string& what) {
	if (!e || !ty) return nullptr;
	if (!ty->isIntegerTy()) return emit(e);

	bool neg = false;
	unsigned long long mag = 0;
	if (evalConstInt(e, neg, mag)) {
		if (neg) { error("Negative constant for unsigned " + what); return nullptr; }
		unsigned bits = ty->getIntegerBitWidth();
		if (bits < 64 && mag > ((1ULL << bits) - 1)) {
			error("Value " + std::to_string(mag) + " is out of range for unsigned " + what);
			return nullptr;
		}
		return llvm::ConstantInt::get(ty, mag, false);
	}

	llvm::Value* v = emit(e);
	if (!v) return nullptr;

	if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1) &&
			!llvm::isa<llvm::Constant>(v) && !isUnsignedExpr(e))
		emitNegativeCheck(v, what);
	return v;
}

void CodeGenerator::emitNegativeCheck(llvm::Value* v, const std::string& what) {
	llvm::Value* isNeg = builder.CreateICmpSLT(v, llvm::ConstantInt::get(v->getType(), 0), "isneg");

	auto* failBB = llvm::BasicBlock::Create(context, "neg_fail", currentFn);
	auto* okBB   = llvm::BasicBlock::Create(context, "neg_ok",   currentFn);
	builder.CreateCondBr(isNeg, failBB, okBB);

	builder.SetInsertPoint(failBB);
	std::string msg = "Runtime Error: negative value assigned to unsigned " + what + "\n";
	auto writeFn = module->getOrInsertFunction("write", llvm::FunctionType::get(
			builder.getInt64Ty(), { builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty() }, false));
	auto exitFn = module->getOrInsertFunction("exit", llvm::FunctionType::get(
			builder.getVoidTy(), { builder.getInt32Ty() }, false));
	llvm::Value* str = builder.CreateGlobalString(msg, "neg_msg");
	builder.CreateCall(writeFn, { builder.getInt32(2), str, builder.getInt64(msg.size()) });
	builder.CreateCall(exitFn, { builder.getInt32(1) });
	builder.CreateUnreachable();

	builder.SetInsertPoint(okBB);
}
