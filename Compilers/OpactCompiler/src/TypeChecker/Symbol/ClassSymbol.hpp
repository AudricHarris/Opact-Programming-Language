#pragma once
#include "Symbol.hpp"
#include "../Types/StructType.hpp"

class ClassSymbol : public Symbol {
	public:
		ClassSymbol(std::string name, std::shared_ptr<StructType> structType)
			: Symbol(std::move(name), structType) {}

		SymbolKind getKind() const override { return SymbolKind::Class; }
};
