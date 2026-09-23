#pragma once
#include "Scope/ScopeManager.hpp"
#include "Types/PrimitiveType.hpp"
#include "Types/FunctionType.hpp"
#include "Types/StructType.hpp"
#include "Symbol/VarSymbol.hpp"
#include "Symbol/FunctionSymbol.hpp"
#include "Symbol/ClassSymbol.hpp"

class TypeChecker {
	public:
		TypeChecker() = default;

		ScopeManager& getScopeManager() { return scopeManager; }

	private:
		ScopeManager scopeManager;
};
