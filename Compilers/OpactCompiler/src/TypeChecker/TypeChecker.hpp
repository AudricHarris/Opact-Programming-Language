// TypeChecker.hpp
#pragma once

#include "../Parser/Ast.hpp"
#include "Scope/ScopeManager.hpp"
#include "Types/Type.hpp"
#include "Types/FunctionType.hpp"
#include "Types/StructType.hpp"
#include <memory>
#include <string>
#include <unordered_set>

class CompilerPipeline;

class TypeChecker : public ASTVisitor {
public:
	TypeChecker(CompilerPipeline* pipeline = nullptr, const std::string& filePath = "");
	
	std::string currentFilePath;
	bool correctCode = true;
	// Main entry point (public API unchanged: returns the AST-level TypeDesc)
	TypeDesc check(Expression& node);
	void ShowErrorCount();
	
	// Declares top-level functions first (so they can be called before their
	// definition), then type checks the whole tree.
	void initialize(const ExprPtr& m);

	[[nodiscard]] bool hasErrors() const { return errorCount_ > 0; }

	// ASTVisitor interface overrides
	void visit(LiteralExpr& node) override;
	void visit(IdentifierExpr& node) override;
	void visit(VarDeclExpr& node) override;
	void visit(BinaryExpr& node) override;
	void visit(UnaryExpr& node) override;
	void visit(Block& node) override;
	void visit(IfExpr& node) override;
	void visit(WhileExpr& node) override;
	void visit(LoopExpr& node) override;
	void visit(FunctionDeclExpr& node) override;
	void visit(StructDeclExpr& node) override;
	void visit(StructCreateExpr& node) override;
	void visit(MemberAccessExpr& node) override;
	void visit(CallExpr& node) override;
	void visit(ReturnExpr& node) override;
	void visit(ImportExpr& node) override;
	void visit(BorrowExpr& node) override;
	void visit(AssignExpr& node) override;
	void visit(PostfixExpr& node) override;
	void visit(CastExpr& node) override;

	// Stub out remaining ASTVisitor pure virtuals...
	void visit(GenericCallExpr& node) override {}
	void visit(ArrayIndexExpr& node) override {}
	void visit(ArrayLiteralExpr& node) override {}
	void visit(TupleLiteralExpr& node) override {}
	void visit(YieldExpr& node) override {}
	void visit(BreakExpr& node) override {}
	void visit(ContinueExpr& node) override {}

private:
	CompilerPipeline* pipeline;
	ScopeManager scopeManager;
	std::shared_ptr<Type> currentType;
	std::shared_ptr<Type> currentReturnType;
	std::unordered_set<const FunctionDeclExpr*> declaredFunctions_;
	std::unordered_set<const StructDeclExpr*> declaredStructures_;
	std::vector<FunctionDeclExpr> persistentBuiltins_;
	int errorCount_ = 0;

	std::shared_ptr<Type> typeOf(Expression& node);
	std::shared_ptr<Type> fromDesc(const TypeDesc& desc);
	TypeDesc toDesc(const std::shared_ptr<Type>& type) const;
	std::shared_ptr<FunctionType> buildFunctionType(const FunctionDeclExpr& fn);
	std::shared_ptr<StructType> buildStructureType(const StructDeclExpr& structure);
	void declareFunction(const FunctionDeclExpr& fn);
	void declareStructures(const StructDeclExpr& structure);

	void declareBuiltins();
	void reportError(const std::string& message);
	bool isWritable(Expression& e);
	std::shared_ptr<Type> valueTypeOf(Expression& node);
};
