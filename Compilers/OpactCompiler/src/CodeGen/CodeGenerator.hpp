#ifndef CODE_GENERATOR_HPP
#define CODE_GENERATOR_HPP

//==========================================================================//
//  CodeGenerator.hpp  -  DECLARATIONS ONLY                                 //
//                                                                          //
//  File layout:                                                            //
//    CodeGenerator.hpp  -> this file: the class + helper declarations      //
//    CodeGenerator.cpp  -> core: types, operators, visitors                //
//    Builtins.cpp       -> builtins: Printf/Println, Range, Random, C libs //
//==========================================================================//

#include "../Parser/Ast.hpp"
#include "../Parser/Parser.hpp"
#include "../Parser/RangeUtil.hpp"
#include "../Lexer/Lexer.hpp"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <cstdint>
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

	CodeGenerator(const std::string& moduleName, llvm::LLVMContext& ctx);

	llvm::Module* getModule() { return module.get(); }
	[[nodiscard]] bool hasErrors() const { return errorCount > 0; }

	//----------------------------------------------------------------------//
	//  Builtins API                                           [Builtins.cpp] //
	//----------------------------------------------------------------------//
	void registerBuiltin(const std::string& name, BuiltinHandler handler);

	// Declares the C function lazily (only when first used) and coerces the
	// arguments to the declared parameter types.
	void registerExternal(const std::string& userName, const std::string& cName,
	                      llvm::Type* ret, std::vector<llvm::Type*> params,
	                      bool variadic = false);

	// Declares the PUBLIC functions of an imported module as external
	// functions so this module can call them. Call before visiting your AST.
	void declareImportedFunctions(Expression* moduleRoot);

	//----------------------------------------------------------------------//
	//  Visitors                                            [CodeGenerator.cpp] //
	//----------------------------------------------------------------------//
	void visit(LiteralExpr& node) override;
	void visit(IdentifierExpr& node) override;
	void visit(VarDeclExpr& node) override;
	void visit(AssignExpr& node) override;
	void visit(PostfixExpr& node) override;
	void visit(BinaryExpr& node) override;
	void visit(UnaryExpr& node) override;
	void visit(CastExpr& node) override;
	void visit(Block& node) override;
	void visit(IfExpr& node) override;
	void visit(WhileExpr& node) override;
	void visit(LoopExpr& node) override;
	void visit(BreakExpr& node) override;
	void visit(ContinueExpr& node) override;
	void visit(ReturnExpr& node) override;
	void visit(FunctionDeclExpr& node) override;
	void visit(StructDeclExpr& node) override;
	void visit(StructCreateExpr&) override;
	void visit(MemberAccessExpr&) override;
	void visit(CallExpr& node) override;
	void visit(ImportExpr& node) override;
	void visit(BorrowExpr& node) override;
	void visit(ArrayIndexExpr& node) override;
	void visit(ArrayLiteralExpr& node) override;

	// Not implemented yet
	void visit(GenericCallExpr&) override {}
	void visit(TupleLiteralExpr&) override {}
	void visit(YieldExpr&) override {}

private:
	//----------------------------------------------------------------------//
	//  Internal data types                                                 //
	//----------------------------------------------------------------------//
	struct VarInfo {
		llvm::Value* addr;
		llvm::Type* type;
		llvm::StructType* structType = nullptr;
		llvm::Type* pointee = nullptr; // set for references to non-struct types (what the pointer points at)
		bool isUnsigned = false;
	};
	struct LoopTargets { llvm::BasicBlock* breakBB; llvm::BasicBlock* continueBB; };
	struct StructInfo {
		llvm::StructType* type;
		std::vector<std::string> fieldNames;
		std::vector<bool> fieldIsRef;
		std::vector<llvm::Type*> fieldPointee;
		std::vector<bool> fieldIsUnsigned;
	};
	struct FnSign {
		std::vector<bool> paramUnsigned;
		bool retUnsigned = false;
	};

	std::vector<std::vector<llvm::Value*>> scopeCleanups;
	std::unordered_map<std::string, FnSign> fnSigns;

	void pushScope() { scopeCleanups.push_back({}); }
	void popScope();
	void registerCleanup(llvm::Value* slot);
	void emitCleanups(const std::vector<llvm::Value*>& slots, llvm::Value* keep);
	llvm::AllocaInst* createHeapSlot(const std::string& name);

	//----------------------------------------------------------------------//
	//  Builtins internals                                     [Builtins.cpp] //
	//----------------------------------------------------------------------//
	void registerBuiltins();   // >>> ADD YOUR BUILTINS HERE <<<
	ExprPtr parseInlineExpression(const std::string& src);
	bool appendFormatArg(llvm::Value*& v, std::string& fmt);
	llvm::Value* emitFormatted(CallExpr& call, bool newline);
	llvm::Value* emitRange(CallExpr& call);
	llvm::Value* emitRandom(CallExpr& call);
	bool constI32(Expression* e, long long& out);

	//----------------------------------------------------------------------//
	//  Type system                                         [CodeGenerator.cpp] //
	//----------------------------------------------------------------------//
	llvm::Type*  mapType(const TypeDesc& d);
	llvm::Value* toBool(llvm::Value* v);
	llvm::Value* coerce(llvm::Value* v, llvm::Type* to);
	llvm::Value* promoteVarArg(llvm::Value* v);
	bool         unify(llvm::Value*& l, llvm::Value*& r);
	llvm::Value* emitBinary(BinaryOp op, llvm::Value* l, llvm::Value* r, bool isUnsigned);
	llvm::Value* emitLValue(Expression* e);
	llvm::Type*  pointeeOf(const TypeDesc& d);

	//----------------------------------------------------------------------//
	//  Unsigned support                                    [CodeGenerator.cpp] //
	//----------------------------------------------------------------------//
	bool parseIntLiteral(LiteralExpr& lit, bool& neg, unsigned long long& mag);
	bool evalConstInt(Expression* e, bool& neg, unsigned long long& mag);
	bool isUnsignedExpr(Expression* e);
	bool unsignedCtx = false;
	llvm::Value* emitUnsignedValue(Expression* e, llvm::Type* ty, const std::string& what);
	void emitNegativeCheck(llvm::Value* v, const std::string& what);

	//----------------------------------------------------------------------//
	//  Helpers                                             [CodeGenerator.cpp] //
	//----------------------------------------------------------------------//
	void error(const std::string& msg);
	llvm::Value* emit(Expression* e);
	llvm::Value* emitRef(Expression* e);   // address of a reference instead of its dereferenced value

	VarInfo* lookupVar(const std::string& name);

	bool blockTerminated();
	void branchIfOpen(llvm::BasicBlock* target);
	void startDeadBlock();
	llvm::AllocaInst* createEntryAlloca(llvm::Function* fn, llvm::Type* ty, const std::string& name);

	llvm::Function* declareFunction(FunctionDeclExpr& node);
	void emitDefaultReturn();
	bool emitFieldAddr(MemberAccessExpr& ma, llvm::Value*& addr, llvm::Type*& type);

	static std::string tokenText(const Token& t) { return t.getWord(); }
	llvm::Type* getExprType(Expression* e);

	//----------------------------------------------------------------------//
	//  State                                                               //
	//----------------------------------------------------------------------//
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
};

#endif // CODE_GENERATOR_HPP
