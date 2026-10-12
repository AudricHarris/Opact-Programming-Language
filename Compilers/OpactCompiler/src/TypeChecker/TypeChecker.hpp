// TypeChecker.hpp
#pragma once

#include "../Parser/Ast.hpp"
#include "Scope/ScopeManager.hpp"
#include "Symbol/VarSymbol.hpp"
#include "Symbol/FunctionSymbol.hpp"
#include "Types/Type.hpp"
#include "Types/PrimitiveType.hpp"
#include "Types/FunctionType.hpp"
#include "Types/StructType.hpp"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class TypeChecker : public ASTVisitor {
public:
	TypeChecker();

	// Main entry point (public API unchanged: returns the AST-level TypeDesc)
	TypeDesc check(Expression& node);

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
	void visit(FunctionDeclExpr& node) override;
	void visit(CallExpr& node) override;
	void visit(ReturnExpr& node) override;

	// Stub out remaining ASTVisitor pure virtuals...
	void visit(PostfixExpr& node) override {}
	void visit(BorrowExpr& node) override {}
	void visit(GenericCallExpr& node) override {}
	void visit(CastExpr& node) override {}
	void visit(AssignExpr& node) override {}
	void visit(LoopExpr& node) override {}
	void visit(MemberAccessExpr& node) override {}
	void visit(ArrayIndexExpr& node) override {}
	void visit(ArrayLiteralExpr& node) override {}
	void visit(TupleLiteralExpr& node) override {}
	void visit(YieldExpr& node) override {}
	void visit(BreakExpr& node) override {}
	void visit(ContinueExpr& node) override {}
	void visit(ImportExpr& node) override {}

private:
	ScopeManager scopeManager;
	std::shared_ptr<Type> currentType;
	std::shared_ptr<Type> currentReturnType;
	std::unordered_set<const FunctionDeclExpr*> declaredFunctions_;
	int errorCount_ = 0;

	std::shared_ptr<Type> typeOf(Expression& node);
	std::shared_ptr<Type> fromDesc(const TypeDesc& desc);
	TypeDesc toDesc(const std::shared_ptr<Type>& type) const;
	std::shared_ptr<FunctionType> buildFunctionType(const FunctionDeclExpr& fn);
	void declareFunction(const FunctionDeclExpr& fn);

	void reportError(const std::string& message);
};
