#pragma once
#include "Symbol.hpp"
#include "../Types/FunctionType.hpp"

class FunctionSymbol : public Symbol {
	public:
		FunctionSymbol(std::string name, std::shared_ptr<FunctionType> fnType)
			: Symbol(std::move(name), fnType) {}

		SymbolKind getKind() const override { return SymbolKind::Function; }
};
