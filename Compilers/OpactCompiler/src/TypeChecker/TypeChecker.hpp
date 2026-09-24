#pragma once
#include "Parser/Ast.hpp"
#include "Scope/ScopeManager.hpp"
#include "Types/PrimitiveType.hpp"
#include "Types/FunctionType.hpp"
#include "Types/StructType.hpp"
#include "Symbol/VarSymbol.hpp"
#include "Symbol/FunctionSymbol.hpp"
#include "Symbol/StructSymbol.hpp"

class TypeChecker {
	public:
		TypeChecker() = default;

		ScopeManager& getScopeManager() { return scopeManager; }

		void initialize(const ExprPtr& m);
	private:
		ScopeManager scopeManager;
};
