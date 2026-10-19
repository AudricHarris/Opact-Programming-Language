#ifndef CODE_GENERATOR_HPP
#define CODE_GENERATOR_HPP

#include "../Parser/Ast.hpp"
#include "../Parser/Parser.hpp"
#include "../Lexer/Lexer.hpp"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

//==========================================================================//
//  Small string helpers (literal handling)                                 //
//==========================================================================//
namespace codegen_detail {

// Removes one pair of surrounding quotes if the lexer kept them.
inline std::string stripQuotes(const std::string& s) {
	if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
	                      (s.front() == '\'' && s.back() == '\''))) {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

// Turns the two characters '\' 'n' into a real newline, etc.
// (harmless if the lexer already did it).
inline std::string unescape(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\\' && i + 1 < s.size()) {
			switch (s[++i]) {
				case 'n':  out += '\n'; break;
				case 't':  out += '\t'; break;
				case 'r':  out += '\r'; break;
				case '0':  out += '\0'; break;
				case '\\': out += '\\'; break;
				case '"':  out += '"';  break;
				case '\'': out += '\''; break;
				default:   out += '\\'; out += s[i]; break;
			}
		} else {
			out += s[i];
		}
	}
	return out;
}

inline std::string trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

} // namespace codegen_detail

//==========================================================================//
//  CodeGenerator                                                           //
//==========================================================================//
class CodeGenerator : public ASTVisitor {
public:
	// A builtin receives the call node, emits IR itself, and returns the
	// resulting value (nullptr for void / on error).
	using BuiltinHandler = std::function<llvm::Value*(CallExpr&)>;

	CodeGenerator(const std::string& moduleName, llvm::LLVMContext& ctx)
		: context(ctx), builder(ctx), module(std::make_unique<llvm::Module>(moduleName, ctx)) {
		scopes.emplace_back(); // global scope
		registerBuiltins();
	}

	llvm::Module* getModule() { return module.get(); }
	[[nodiscard]] bool hasErrors() const { return errorCount > 0; }

	//======================================================================//
	//  >>> ADD YOUR BUILTINS HERE <<<                                      //
	//======================================================================//
private:
	void registerBuiltins() {
		llvm::Type* i32 = builder.getInt32Ty();
		llvm::Type* ptr = llvm::PointerType::get(builder.getInt8Ty(), 0);
		llvm::Type* voidTy = builder.getVoidTy();

		// 1) Custom builtins: full control over the emitted IR.
		registerBuiltin("Printf",  [this](CallExpr& c) { return emitFormatted(c, /*newline=*/false); });
		registerBuiltin("Println", [this](CallExpr& c) { return emitFormatted(c, /*newline=*/true);  });

		// 2) Thin wrappers over C library functions: ONE line each.
		//                  user name, C name, return, params,   variadic
		registerExternal("Exit", "exit", voidTy, { i32 });
		registerExternal("Puts", "puts", i32,    { ptr });
		registerExternal("Abs",  "abs",  i32,    { i32 });
	}

public:
	void registerBuiltin(const std::string& name, BuiltinHandler handler) {
		builtins[name] = std::move(handler);
	}

	// Declares the C function lazily (only when first used) and coerces the
	// arguments to the declared parameter types.
	void registerExternal(const std::string& userName, const std::string& cName,
			llvm::Type* ret, std::vector<llvm::Type*> params, bool variadic = false) {
		registerBuiltin(userName, [this, userName, cName, ret, params, variadic](CallExpr& call) -> llvm::Value* {
				bool badCount = variadic ? call.arguments.size() < params.size()
				: call.arguments.size() != params.size();
				if (badCount) {
				error(userName + ": expected " + std::to_string(params.size()) + " argument(s), got " +
						std::to_string(call.arguments.size()));
				return nullptr;
				}

				auto callee = module->getOrInsertFunction(cName, llvm::FunctionType::get(ret, params, variadic));

				std::vector<llvm::Value*> args;
				for (size_t i = 0; i < call.arguments.size(); ++i) {
				llvm::Value* v = emit(call.arguments[i].get());
				if (!v) return nullptr;
				v = (i < params.size()) ? coerce(v, params[i]) : promoteVarArg(v);
				if (!v) return nullptr;
				args.push_back(v);
				}
				if (ret->isVoidTy()) return builder.CreateCall(callee, args);
				return builder.CreateCall(callee, args, "calltmp");
		});
	}

	//======================================================================//
	//  Type system (single place to extend when adding types)              //
	//======================================================================//
private:
	llvm::Type* mapType(const TypeDesc& d) {
		switch (d.type) {
			case DataType::Int:   return builder.getInt32Ty();
			case DataType::Float: return builder.getDoubleTy();
			case DataType::Bool:  return builder.getInt1Ty();
			case DataType::Char:  return builder.getInt8Ty();
			case DataType::Str:   return llvm::PointerType::get(builder.getInt8Ty(), 0);
			case DataType::Void:  return builder.getVoidTy();
			case DataType::Custom: {
									   auto it = structs.find(d.customTypeName);
									   if (it != structs.end()) return it->second.type;
									   error("Unknown type '" + d.customTypeName + "'");
									   return nullptr;
								   }
		}
		return nullptr;
	}

	// Anything -> i1 (non-zero / non-null is true).
	llvm::Value* toBool(llvm::Value* v) {
		if (!v) return nullptr;
		llvm::Type* t = v->getType();
		if (t->isIntegerTy(1)) return v;
		if (t->isFloatingPointTy())
			return builder.CreateFCmpUNE(v, llvm::ConstantFP::get(t, 0.0), "tobool");
		return builder.CreateIsNotNull(v, "tobool"); // ints and pointers
	}

	// Converts v to type `to` (int widths, int<->float, float widths, ->bool).
llvm::Value* coerce(llvm::Value* v, llvm::Type* to) {
		if (!v || !to) return nullptr;
		llvm::Type* from = v->getType();
		if (from == to) return v;
		if (to->isIntegerTy(1)) return toBool(v);

		if (from->isIntegerTy() && to->isIntegerTy()) {
			unsigned fb = from->getIntegerBitWidth(), tb = to->getIntegerBitWidth();
			if (tb < fb) return builder.CreateTrunc(v, to, "trunc");
			return fb == 1 ? builder.CreateZExt(v, to, "zext") : builder.CreateSExt(v, to, "sext");
		}
		if (from->isIntegerTy() && to->isFloatingPointTy())
			return from->isIntegerTy(1) ? builder.CreateUIToFP(v, to, "uitofp")
			                            : builder.CreateSIToFP(v, to, "sitofp");
		if (from->isFloatingPointTy() && to->isIntegerTy())
			return builder.CreateFPToSI(v, to, "fptosi");
		if (from->isFloatingPointTy() && to->isFloatingPointTy())
			return from->getPrimitiveSizeInBits() < to->getPrimitiveSizeInBits()
				? builder.CreateFPExt(v, to, "fpext") : builder.CreateFPTrunc(v, to, "fptrunc");

		// >>> ADD THIS BLOCK TO HANDLE POINTER / STRING LITERAL COERCION <<<
		if (from->isPointerTy() && to->isPointerTy()) {
			// If it's an array pointer ([N x i8]* -> i8*), get pointer to first element
			if (auto* ptrType = llvm::dyn_cast<llvm::PointerType>(from)) {
				// Check if the underlying type is an array (e.g. string literals)
				// Or use a direct pointer cast / GEP decay
			}
			return builder.CreatePointerCast(v, to, "ptrcast");
		}
		// =================================================================

		error("Cannot convert between these types");
		return nullptr;
	}

	// C variadic promotion: small ints -> i32 (floats are already double).
	llvm::Value* promoteVarArg(llvm::Value* v) {
		llvm::Type* t = v->getType();
		if (t->isIntegerTy() && t->getIntegerBitWidth() < 32) return coerce(v, builder.getInt32Ty());
		if (t->isFloatTy()) return builder.CreateFPExt(v, builder.getDoubleTy(), "fpext");
		return v;
	}

	//======================================================================//
	//  Printf / Println                                                    //
	//======================================================================//

	// Parses `src` as a single expression using the normal Lexer + Parser.
	ExprPtr parseInlineExpression(const std::string& src) {
		try {
			Lexer lexer(src);
			std::vector<Token> toks = lexer.Tokenize();
			Parser parser(std::move(toks), "<format>", nullptr);
			return parser.parseExpression();
		} catch (...) {
			return nullptr;
		}
	}

	// Picks the printf specifier from the LLVM type and adjusts the value for varargs.
	bool appendFormatArg(llvm::Value*& v, std::string& fmt) {
		llvm::Type* t = v->getType();
		if (t->isIntegerTy(1)) {            // bool -> "true"/"false"
			fmt += "%s";
			v = builder.CreateSelect(v, builder.CreateGlobalString("true"),
					builder.CreateGlobalString("false"), "boolstr");
			return true;
		}
		if (t->isIntegerTy(8))  { fmt += "%c";   v = coerce(v, builder.getInt32Ty()); return true; }
		if (t->isIntegerTy(32)) { fmt += "%d";   return true; }
		if (t->isIntegerTy())   { fmt += "%lld"; v = coerce(v, builder.getInt64Ty()); return true; }
		if (t->isFloatingPointTy()) { fmt += "%f"; v = promoteVarArg(v); return true; }
		if (t->isPointerTy())   { fmt += "%s";   return true; }
		error("Printf: cannot format a value of this type");
		return false;
	}

	// Printf("x = {x}, sum = {a + b}, positional = {}\n", extraArg)
	llvm::Value* emitFormatted(CallExpr& call, bool newline) {
		using namespace codegen_detail;

		if (call.arguments.empty()) { error("Printf needs a format string"); return nullptr; }
		auto* lit = dynamic_cast<LiteralExpr*>(call.arguments[0].get());
		if (!lit || lit->kind != LiteralKind::Str) {
			error("Printf: first argument must be a string literal");
			return nullptr;
		}

		std::string tmpl = unescape(stripQuotes(tokenText(lit->lit)));
		if (newline) tmpl += '\n';

		std::string fmt;
		std::vector<llvm::Value*> args;
		size_t nextPositional = 1;

		for (size_t i = 0; i < tmpl.size(); ++i) {
			char c = tmpl[i];

			if (c == '{') {
				if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { fmt += '{'; ++i; continue; }

				// find the matching '}' (braces may nest)
				size_t depth = 1, j = i + 1;
				for (; j < tmpl.size(); ++j) {
					if (tmpl[j] == '{') ++depth;
					else if (tmpl[j] == '}' && --depth == 0) break;
				}
				if (j >= tmpl.size()) { error("Printf: unterminated '{' in format string"); return nullptr; }

				std::string inner = trim(tmpl.substr(i + 1, j - i - 1));
				llvm::Value* v = nullptr;

				if (inner.empty()) {                       // "{}" -> next extra argument
					if (nextPositional >= call.arguments.size()) {
						error("Printf: '{}' has no matching argument");
						return nullptr;
					}
					v = emit(call.arguments[nextPositional++].get());
				} else {                                   // "{any expression}"
					ExprPtr e = parseInlineExpression(inner);
					if (!e) { error("Printf: invalid expression '" + inner + "'"); return nullptr; }
					v = emit(e.get());
				}

				if (!v || !appendFormatArg(v, fmt)) return nullptr;
				args.push_back(v);
				i = j;
			} else if (c == '}') {
				if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') ++i;
				fmt += '}';
			} else if (c == '%') {
				fmt += "%%";
			} else {
				fmt += c;
			}
		}

		if (nextPositional < call.arguments.size())
			std::cerr << "Codegen Warning: Printf has unused extra arguments\n";

		// Declare printf with a pointer type matching your LLVM context
		llvm::Type* charPtrTy = llvm::PointerType::get(builder.getInt8Ty(), 0);
		auto printfFn = module->getOrInsertFunction("printf",
				llvm::FunctionType::get(builder.getInt32Ty(), { charPtrTy }, true));

		// Create the global string array
		llvm::Value* formatStr = builder.CreateGlobalString(fmt, "fmt");

		// Get the pointer to the first element (decays [N x i8]* to a pointer)
		llvm::Value* formatPtr = builder.CreateInBoundsGEP(
				formatStr->getType()->getContainedType(0),
				formatStr,
				{ builder.getInt32(0), builder.getInt32(0) },
				"fmt_ptr"
				);

		// Ensure the pointer type matches what the builder / module expects (fixes typed vs opaque mismatch)
		llvm::Value* castedFormatPtr = builder.CreatePointerCast(formatPtr, charPtrTy, "fmt_cast");

		args.insert(args.begin(), castedFormatPtr);
		return builder.CreateCall(printfFn, args, "printf_ret");
	}

	//======================================================================//
	//  Internals                                                           //
	//======================================================================//
	struct VarInfo { llvm::Value* addr; llvm::Type* type; };
	struct LoopTargets { llvm::BasicBlock* breakBB; llvm::BasicBlock* continueBB; };
	struct StructInfo { llvm::StructType* type; std::vector<std::string> fieldNames; };

	llvm::LLVMContext& context;
	llvm::IRBuilder<> builder;
	std::unique_ptr<llvm::Module> module;

	llvm::Value* lastValue = nullptr;
	llvm::Function* currentFn = nullptr;
	int errorCount = 0;

	std::vector<std::unordered_map<std::string, VarInfo>> scopes;
	std::vector<LoopTargets> loopStack;
	std::unordered_map<std::string, BuiltinHandler> builtins;
	std::unordered_map<std::string, StructInfo> structs;

	static std::string tokenText(const Token& t) { return t.getWord(); }

	void error(const std::string& msg) {
		++errorCount;
		std::cerr << "Codegen Error: " << msg << "\n";
	}

	// Evaluate an expression and return its value (nullptr if none / error).
	llvm::Value* emit(Expression* e) {
		if (!e) return nullptr;
		lastValue = nullptr;
		e->accept(*this);
		return lastValue;
	}

	void pushScope() { scopes.emplace_back(); }
	void popScope()  { scopes.pop_back(); }

	VarInfo* lookupVar(const std::string& name) {
		for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
			auto f = it->find(name);
			if (f != it->end()) return &f->second;
		}
		return nullptr;
	}

	// Don't call BasicBlock::getTerminator() here: newer LLVM asserts on blocks
	// that are still being built (empty / no terminator yet).
	bool blockTerminated() {
		llvm::BasicBlock* bb = builder.GetInsertBlock();
		return bb && !bb->empty() && bb->back().isTerminator();
	}

	void branchIfOpen(llvm::BasicBlock* target) {
		if (!blockTerminated()) builder.CreateBr(target);
	}

	// After return/break/continue: anything emitted next goes into an unreachable block.
	void startDeadBlock() {
		builder.SetInsertPoint(llvm::BasicBlock::Create(context, "dead", currentFn));
	}

	llvm::AllocaInst* createEntryAlloca(llvm::Function* fn, llvm::Type* ty, const std::string& name) {
		llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
		return tmp.CreateAlloca(ty, nullptr, name);
	}

	// Brings both operands to a common numeric type.
	bool unify(llvm::Value*& l, llvm::Value*& r) {
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
		error("Operand types do not match");
		return false;
	}

	llvm::Value* emitBinary(BinaryOp op, llvm::Value* l, llvm::Value* r) {
		if (op == BinaryOp::And) return builder.CreateAnd(toBool(l), toBool(r), "andtmp");
		if (op == BinaryOp::Or)  return builder.CreateOr(toBool(l),  toBool(r), "ortmp");

		llvm::Type* lt = l->getType();
		llvm::Type* rt = r->getType();

		// Handle string/pointer concatenation BEFORE unify so different pointer types don't trigger errors
		if (op == BinaryOp::Add && !lt->isFloatingPointTy() && !lt->isIntegerTy() && !rt->isFloatingPointTy() && !rt->isIntegerTy()) {
			auto charPtrTy = llvm::PointerType::get(builder.getInt8Ty(), 0);
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
				"totallen"
			);
			llvm::Value* dest = builder.CreateCall(mallocFn, { totalLen }, "dest");
			builder.CreateCall(memcpyFn, { dest, str1, len1 });
			llvm::Value* destOffset = builder.CreateInBoundsGEP(builder.getInt8Ty(), dest, { len1 }, "dest_offset");
			llvm::Value* len2Plus1 = builder.CreateAdd(len2, llvm::ConstantInt::get(sizeTy, 1), "len2_plus_1");
			builder.CreateCall(memcpyFn, { destOffset, str2, len2Plus1 });
			return dest;
		}

		if (!unify(l, r)) return nullptr;
		bool fp = lt->isFloatingPointTy();

		switch (op) {
			case BinaryOp::Add: return fp ? builder.CreateFAdd(l, r, "addtmp") : builder.CreateAdd(l, r, "addtmp");
			case BinaryOp::Sub: return fp ? builder.CreateFSub(l, r, "subtmp") : builder.CreateSub(l, r, "subtmp");
			case BinaryOp::Mul: return fp ? builder.CreateFMul(l, r, "multmp") : builder.CreateMul(l, r, "multmp");
			case BinaryOp::Div: return fp ? builder.CreateFDiv(l, r, "divtmp") : builder.CreateSDiv(l, r, "divtmp");
			case BinaryOp::Mod: return fp ? builder.CreateFRem(l, r, "modtmp") : builder.CreateSRem(l, r, "modtmp");

			case BinaryOp::Power: {
				llvm::Type* d = builder.getDoubleTy();
				llvm::Value* res = builder.CreateIntrinsic(llvm::Intrinsic::pow, { d },
				                                           { coerce(l, d), coerce(r, d) }, nullptr, "powtmp");
				std::cout << "Got Power\n";
				return fp ? res : coerce(res, lt);
			}

			case BinaryOp::BitAnd:
				if (fp) { error("Bitwise operator needs integer operands"); return nullptr; }
				return builder.CreateAnd(l, r, "bandtmp");

			case BinaryOp::Eq: return fp ? builder.CreateFCmpOEQ(l, r, "cmptmp") : builder.CreateICmpEQ(l, r, "cmptmp");
			case BinaryOp::Ne: return fp ? builder.CreateFCmpUNE(l, r, "cmptmp") : builder.CreateICmpNE(l, r, "cmptmp");
			case BinaryOp::Lt: return fp ? builder.CreateFCmpOLT(l, r, "cmptmp") : builder.CreateICmpSLT(l, r, "cmptmp");
			case BinaryOp::Gt: return fp ? builder.CreateFCmpOGT(l, r, "cmptmp") : builder.CreateICmpSGT(l, r, "cmptmp");
			case BinaryOp::Le: return fp ? builder.CreateFCmpOLE(l, r, "cmptmp") : builder.CreateICmpSLE(l, r, "cmptmp");
			case BinaryOp::Ge: return fp ? builder.CreateFCmpOGE(l, r, "cmptmp") : builder.CreateICmpSGE(l, r, "cmptmp");
			default: break;
		}
		error("Unsupported binary operator");
		return nullptr;
	}

	// ----- functions ------------------------------------------------------
	llvm::Function* declareFunction(FunctionDeclExpr& node) {
		std::string name = (node.name == "Main") ? "main" : node.name;
		if (auto* existing = module->getFunction(name)) return existing;

		// main must return int even if the source omitted the return type
		llvm::Type* ret = (name == "main") ? builder.getInt32Ty() : mapType(node.returnType);
		if (!ret) return nullptr;

		std::vector<llvm::Type*> params;
		for (const auto& p : node.params) {
			llvm::Type* pt = mapType(p.type);
			if (!pt || pt->isVoidTy()) { error("Invalid type for parameter '" + p.name + "'"); return nullptr; }
			params.push_back(pt);
		}

		auto* fnType = llvm::FunctionType::get(ret, params, false);
		return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, module.get());
	}

	void emitDefaultReturn() {
		if (blockTerminated()) return;
		llvm::Type* rt = currentFn->getReturnType();
		if (rt->isVoidTy()) builder.CreateRetVoid();
		else builder.CreateRet(llvm::Constant::getNullValue(rt));
	}

public:
	//---------------------------------//
	//-- Visitor Implementations ------//
	//---------------------------------//

	void visit(LiteralExpr& node) override {
		using namespace codegen_detail;
		std::string text = tokenText(node.lit);

		switch (node.kind) {
			case LiteralKind::Int: {
									   text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
									   int base = 10;
									   size_t skip = 0;
									   switch (node.base) {
										   case NumericBase::Hexadecimal: base = 16; skip = 2; break;
										   case NumericBase::Octal:       base = 8;  skip = 2; break;
										   case NumericBase::Binary:      base = 2;  skip = 2; break;
										   default: break;
									   }
									   try {
										   unsigned long long v = std::stoull(text.substr(skip), nullptr, base);
										   llvm::Type* ty = (v > 0x7FFFFFFFULL) ? builder.getInt64Ty() : builder.getInt32Ty();
										   lastValue = llvm::ConstantInt::get(ty, v, false);
									   } catch (...) {
										   error("Invalid integer literal '" + text + "'");
										   lastValue = nullptr;
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
										std::string c = unescape(stripQuotes(text));
										lastValue = builder.getInt8(c.empty() ? 0 : static_cast<uint8_t>(c[0]));
										break;
									}
			case LiteralKind::Str:
									lastValue = builder.CreateGlobalString(unescape(stripQuotes(text)), "str");
									break;
		}
	}

	void visit(IdentifierExpr& node) override {
		VarInfo* var = lookupVar(node.name);
		if (!var) {
			error("Undefined variable '" + node.name + "'");
			lastValue = nullptr;
			return;
		}
		lastValue = builder.CreateLoad(var->type, var->addr, node.name);
	}

	void visit(VarDeclExpr& node) override {
		std::string name = tokenText(node.name);
		if (!currentFn) { error("Variable '" + name + "' declared outside of a function"); return; }

		llvm::Value* init = node.expr ? emit(node.expr.get()) : nullptr;
		if (node.expr && !init) { lastValue = nullptr; return; }

		// Void here means "no explicit type": infer from the initializer.
		llvm::Type* ty = (node.type.type == DataType::Void && init) ? init->getType() : mapType(node.type);
		if (!ty || ty->isVoidTy()) { error("Invalid type for variable '" + name + "'"); return; }

		llvm::Value* initVal = init ? coerce(init, ty) : llvm::Constant::getNullValue(ty);
		if (!initVal) return;

		llvm::AllocaInst* slot = createEntryAlloca(currentFn, ty, name);
		builder.CreateStore(initVal, slot);
		scopes.back()[name] = { slot, ty };
		lastValue = initVal;
	}

	void visit(AssignExpr& node) override {
		auto* id = dynamic_cast<IdentifierExpr*>(node.target.get());
		if (!id) { error("Only variables can be assigned to (for now)"); return; }
		VarInfo* var = lookupVar(id->name);
		if (!var) { error("Undefined variable '" + id->name + "'"); return; }

		llvm::Value* v = coerce(emit(node.value.get()), var->type);
		if (!v) { lastValue = nullptr; return; }
		builder.CreateStore(v, var->addr);
		lastValue = v;
	}

	void visit(PostfixExpr& node) override {
		auto* id = dynamic_cast<IdentifierExpr*>(node.expr.get());
		VarInfo* var = id ? lookupVar(id->name) : nullptr;
		if (!var) { error("++/-- needs a variable"); return; }

		llvm::Value* old = builder.CreateLoad(var->type, var->addr, "old");
		bool inc = node.op == PostfixOp::Increment;
		llvm::Value* updated;
		if (var->type->isFloatingPointTy()) {
			llvm::Value* one = llvm::ConstantFP::get(var->type, 1.0);
			updated = inc ? builder.CreateFAdd(old, one, "inc") : builder.CreateFSub(old, one, "dec");
		} else if (var->type->isIntegerTy()) {
			llvm::Value* one = llvm::ConstantInt::get(var->type, 1);
			updated = inc ? builder.CreateAdd(old, one, "inc") : builder.CreateSub(old, one, "dec");
		} else {
			error("++/-- needs a numeric variable");
			return;
		}
		builder.CreateStore(updated, var->addr);
		lastValue = old; // postfix yields the previous value
	}

	void visit(BinaryExpr& node) override {
		llvm::Value* l = emit(node.left.get());
		llvm::Value* r = emit(node.right.get());
		lastValue = (l && r) ? emitBinary(node.op, l, r) : nullptr;
	}

	void visit(UnaryExpr& node) override {
		llvm::Value* v = emit(node.expr.get());
		if (!v) { lastValue = nullptr; return; }

		if (node.op == UnaryOp::Negate) {
			lastValue = v->getType()->isFloatingPointTy() ? builder.CreateFNeg(v, "negtmp")
				: builder.CreateNeg(v, "negtmp");
		} else {
			lastValue = builder.CreateNot(toBool(v), "nottmp");
		}
	}

	void visit(CastExpr& node) override {
		llvm::Value* v = emit(node.expr.get());
		llvm::Type* to = mapType(node.castType);
		lastValue = (v && to && !to->isVoidTy()) ? coerce(v, to) : nullptr;
	}

	void visit(Block& node) override {
		// At module level: register structs and function prototypes first so
		// declaration order does not matter.
		if (!currentFn) {
			for (auto& e : node.expressions) {
				if (auto* s = dynamic_cast<StructDeclExpr*>(e.get()); s && !structs.count(s->name))
					structs[s->name] = { llvm::StructType::create(context, s->name), {} };
			}
			for (auto& e : node.expressions) {
				if (auto* f = dynamic_cast<FunctionDeclExpr*>(e.get())) declareFunction(*f);
			}
		}

		pushScope();
		llvm::Value* last = nullptr;
		for (auto& e : node.expressions) {
			if (e) last = emit(e.get());
		}
		popScope();
		lastValue = last;
	}

	void visit(IfExpr& node) override {
		llvm::Value* cond = toBool(emit(node.condition.get()));
		if (!cond || !node.ifBranch) return;

		auto* thenBB  = llvm::BasicBlock::Create(context, "then", currentFn);
		auto* elseBB  = (node.elseBranch && *node.elseBranch) ? llvm::BasicBlock::Create(context, "else", currentFn) : nullptr;
		auto* mergeBB = llvm::BasicBlock::Create(context, "ifcont", currentFn);

		builder.CreateCondBr(cond, thenBB, elseBB ? elseBB : mergeBB);

		builder.SetInsertPoint(thenBB);
		emit(node.ifBranch.get());
		branchIfOpen(mergeBB);

		if (elseBB) {
			builder.SetInsertPoint(elseBB);
			emit(node.elseBranch->get());
			branchIfOpen(mergeBB);
		}

		builder.SetInsertPoint(mergeBB);
		lastValue = nullptr;
	}

	void visit(WhileExpr& node) override {
		if (!node.condition || !node.loopBranch) return;

		auto* condBB  = llvm::BasicBlock::Create(context, "whilecond",  currentFn);
		auto* loopBB  = llvm::BasicBlock::Create(context, "whileloop",  currentFn);
		auto* afterBB = llvm::BasicBlock::Create(context, "whileafter", currentFn);

		builder.CreateBr(condBB);
		builder.SetInsertPoint(condBB);
		llvm::Value* cond = toBool(emit(node.condition.get()));
		if (!cond) return;
		builder.CreateCondBr(cond, loopBB, afterBB);

		builder.SetInsertPoint(loopBB);
		loopStack.push_back({ afterBB, condBB });
		emit(node.loopBranch.get());
		loopStack.pop_back();
		branchIfOpen(condBB);

		builder.SetInsertPoint(afterBB);
		lastValue = nullptr;
	}

	void visit(LoopExpr& node) override {
		if (!node.loopBranch) return;

		auto* bodyBB  = llvm::BasicBlock::Create(context, "loop",      currentFn);
		auto* afterBB = llvm::BasicBlock::Create(context, "loopafter", currentFn);

		builder.CreateBr(bodyBB);
		builder.SetInsertPoint(bodyBB);
		loopStack.push_back({ afterBB, bodyBB });
		emit(node.loopBranch.get());
		loopStack.pop_back();
		branchIfOpen(bodyBB);

		builder.SetInsertPoint(afterBB);
		lastValue = nullptr;
	}

	void visit(BreakExpr&) override {
		if (loopStack.empty()) { error("'break' outside of a loop"); return; }
		builder.CreateBr(loopStack.back().breakBB);
		startDeadBlock();
	}

	void visit(ContinueExpr&) override {
		if (loopStack.empty()) { error("'continue' outside of a loop"); return; }
		builder.CreateBr(loopStack.back().continueBB);
		startDeadBlock();
	}

	void visit(ReturnExpr& node) override {
		if (!currentFn) { error("'return' outside of a function"); return; }

		llvm::Value* v = (node.value && *node.value) ? emit(node.value->get()) : nullptr;
		llvm::Type* rt = currentFn->getReturnType();

		if (rt->isVoidTy()) {
			builder.CreateRetVoid();
		} else {
			v = v ? coerce(v, rt) : llvm::Constant::getNullValue(rt);
			if (!v) return;
			builder.CreateRet(v);
		}
		startDeadBlock();
		lastValue = nullptr;
	}

	void visit(FunctionDeclExpr& node) override {
		llvm::Function* fn = declareFunction(node);
		if (!fn || !node.body) return;
		if (!fn->empty()) { error("Redefinition of function '" + node.name + "'"); return; }

		// Save state so nested declarations don't corrupt the outer function.
		auto savedIP   = builder.saveIP();
		auto* savedFn  = currentFn;
		auto savedLoop = std::move(loopStack);
		loopStack.clear();

		currentFn = fn;
		builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));

		pushScope();
		size_t idx = 0;
		for (auto& arg : fn->args()) {
			const std::string& pname = node.params[idx++].name;
			arg.setName(pname);
			llvm::AllocaInst* slot = createEntryAlloca(fn, arg.getType(), pname);
			builder.CreateStore(&arg, slot);
			scopes.back()[pname] = { slot, arg.getType() };
		}

		emit(node.body.get());
		emitDefaultReturn();
		popScope();

		currentFn = savedFn;
		loopStack = std::move(savedLoop);
		builder.restoreIP(savedIP);
		lastValue = nullptr;
	}

	void visit(StructDeclExpr& node) override {
		auto it = structs.find(node.name);
		if (it == structs.end())
			it = structs.emplace(node.name, StructInfo{ llvm::StructType::create(context, node.name), {} }).first;

		std::vector<llvm::Type*> fieldTypes;
		for (const auto& f : node.attributes) {
			llvm::Type* ft = mapType(f.type);
			if (!ft || ft->isVoidTy()) { error("Invalid type for field '" + f.name + "'"); return; }
			fieldTypes.push_back(ft);
			it->second.fieldNames.push_back(f.name);
		}
		if (it->second.type->isOpaque()) it->second.type->setBody(fieldTypes);
	}

	void visit(CallExpr& node) override {
		lastValue = nullptr;
		auto* id = dynamic_cast<IdentifierExpr*>(node.callee.get());
		if (!id) { error("Only direct calls by name are supported"); return; }

		// 1) user-defined function (user code wins over builtins)
		llvm::Function* callee = module->getFunction(id->name == "Main" ? "main" : id->name);
		if (callee) {
			if (callee->arg_size() != node.arguments.size()) {
				error("Function '" + id->name + "' expects " + std::to_string(callee->arg_size()) +
						" argument(s), got " + std::to_string(node.arguments.size()));
				return;
			}
			std::vector<llvm::Value*> args;
			size_t i = 0;
			for (auto& a : node.arguments) {
				llvm::Value* v = coerce(emit(a.get()), callee->getFunctionType()->getParamType(i++));
				if (!v) return;
				args.push_back(v);
			}
			lastValue = callee->getReturnType()->isVoidTy() ? builder.CreateCall(callee, args)
				: builder.CreateCall(callee, args, "calltmp");
			return;
		}

		// 2) builtin
		auto b = builtins.find(id->name);
		if (b != builtins.end()) {
			lastValue = b->second(node);
			return;
		}

		error("Unknown function '" + id->name + "'");
	}

	void visit(ImportExpr&) override { /* handled by the compiler pipeline */ }

	// Not implemented yet
	void visit(BorrowExpr&) override {}
	void visit(GenericCallExpr&) override {}
	void visit(StructCreateExpr&) override {}
	void visit(MemberAccessExpr&) override {}
	void visit(ArrayIndexExpr&) override {}
	void visit(ArrayLiteralExpr&) override {}
	void visit(TupleLiteralExpr&) override {}
	void visit(YieldExpr&) override {}
};

#endif // CODE_GENERATOR_HPP
