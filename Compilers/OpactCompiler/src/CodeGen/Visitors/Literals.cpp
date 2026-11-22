//==========================================================================//
//  Literals.cpp  -  visit(LiteralExpr), visit(IdentifierExpr)              //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(LiteralExpr& node) {
	using namespace codegen_detail;
	std::string text = tokenText(node.lit);

	switch (node.kind) {
		case LiteralKind::Int: {
			bool neg = false;
			unsigned long long mag = 0;
			if (!parseIntLiteral(node, neg, mag)) { lastValue = nullptr; break; }

			if (neg) {
				long long sv = -static_cast<long long>(mag);
				llvm::Type* ty = (sv >= INT32_MIN) ? builder.getInt32Ty() : builder.getInt64Ty();
				lastValue = llvm::ConstantInt::get(ty, static_cast<uint64_t>(sv), /*isSigned=*/true);
			} else {
				llvm::Type* ty = (mag > 0x7FFFFFFFULL) ? builder.getInt64Ty() : builder.getInt32Ty();
				lastValue = llvm::ConstantInt::get(ty, mag, false);
			}
			break;
		}
		case LiteralKind::Float:
			try {
				lastValue = llvm::ConstantFP::get(builder.getDoubleTy(), std::stod(text));
			} catch (...) {
				error("Invalid float literal '" + text + "'");
				lastValue = nullptr;
			}
			break;
		case LiteralKind::Bool:
			lastValue = builder.getInt1(text == "true");
			break;
		case LiteralKind::Char: {
			std::string c = unescape(expandAnsiEscapes(stripQuotes(text)));
			lastValue = builder.getInt8(c.empty() ? 0 : static_cast<uint8_t>(c[0]));
			break;
		}
		case LiteralKind::Str:
			lastValue = builder.CreateGlobalString(unescape(expandAnsiEscapes(stripQuotes(text))), "str");
			break;
	}
}

void CodeGenerator::visit(IdentifierExpr& node) {
	VarInfo* var = lookupVar(node.name);
	if (!var) {
		error("Undefined variable '" + node.name + "'");
		lastValue = nullptr;
		return;
	}
	if (var->pointee) {
		// Reference to a primitive: load the pointer, then the value behind it.
		llvm::Value* p = builder.CreateLoad(builder.getPtrTy(), var->addr, node.name + "_ref");
		lastValue = builder.CreateLoad(var->pointee, p, node.name);
		return;
	}
	lastValue = builder.CreateLoad(var->type, var->addr, node.name);
}
