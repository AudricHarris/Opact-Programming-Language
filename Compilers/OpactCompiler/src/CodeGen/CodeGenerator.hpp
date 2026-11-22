#ifndef CODE_GENERATOR_HPP
#define CODE_GENERATOR_HPP

//==========================================================================//
//  CodeGenerator.hpp  -  DECLARATIONS ONLY                                 //
//                                                                          //
//  File layout (see docs/ADDING_FEATURES.md):                              //
//    CodeGenerator.hpp   this file: the class + all declarations           //
//    StringUtil.*        literal helpers (unescape, \e, \x1b ...)          //
//    DynArray.*          '?'-dimension array descriptors                   //
//    Core/               types, scopes, functions, unsigned, lvalues       //
//    Visitors/           one file per group of AST nodes                   //
//    Builtins/           Printf, Range, Random, Read, Libc, Math ...       //
//==========================================================================//

#include "../Parser/Ast.hpp"
#include "../Parser/Parser.hpp"
#include "../Parser/RangeUtil.hpp"
#include "../Lexer/Lexer.hpp"

#include "DynArray.hpp"
#include "StringUtil.hpp"

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
	//  Builtins API                                            [Builtins/] //
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
	//  Visitors                                         [Core/, Visitors/] //
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
	void visit(ForExpr& node) override;
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
	void visit(ArrayAllocExpr& node) override;

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
		llvm::Value* ownedSlot = nullptr; // heap slot currently owned by this variable
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
	void transferHeapSlot(VarInfo* var, llvm::Value* slot);

	//----------------------------------------------------------------------//
	//  Builtins internals                                      [Builtins/] //
	//----------------------------------------------------------------------//
	void registerBuiltins();        // Builtins/Builtins.cpp: calls every module below
	// One register function per module (each lives in Builtins/<Module>.cpp):
	void registerFormatBuiltins();  // Printf, Println
	void registerRangeBuiltins();   // Range
	void registerRandomBuiltins();  // Random
	void registerReadBuiltins();    // Read
	void registerLibcBuiltins();    // Exit, Puts, Abs
	void registerMathBuiltins();    // Sqrt, Pow, Min, Max  (example module)
	// >>> DECLARE YOUR NEW MODULE HERE: void registerYourBuiltins(); <<<
	ExprPtr parseInlineExpression(const std::string& src);
	bool appendFormatArg(llvm::Value*& v, std::string& fmt);
	llvm::Value* emitFormatted(CallExpr& call, bool newline);
	llvm::Value* emitRange(CallExpr& call);
	llvm::Value* emitRandom(CallExpr& call);
	llvm::Value* emitRead(CallExpr& call);
	bool constI32(Expression* e, long long& out);

	//----------------------------------------------------------------------//
	//  Type system                                      [Core/, Visitors/] //
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
	//  Unsigned support                                 [Core/, Visitors/] //
	//----------------------------------------------------------------------//
	bool parseIntLiteral(LiteralExpr& lit, bool& neg, unsigned long long& mag);
	bool evalConstInt(Expression* e, bool& neg, unsigned long long& mag);
	bool isUnsignedExpr(Expression* e);
	bool unsignedCtx = false;
	llvm::Value* emitUnsignedValue(Expression* e, llvm::Type* ty, const std::string& what);
	void emitNegativeCheck(llvm::Value* v, const std::string& what);

	//----------------------------------------------------------------------//
	//  Helpers                                          [Core/, Visitors/] //
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
	llvm::Value* lastHeapSlot = nullptr;
	llvm::Function* currentFn = nullptr;
	int errorCount = 0;

	std::vector<std::unordered_map<std::string, VarInfo>> scopes;
	std::vector<LoopTargets> loopStack;
	std::unordered_map<std::string, BuiltinHandler> builtins;
	std::unordered_map<std::string, StructInfo> structs;
};

#endif // CODE_GENERATOR_HPP
