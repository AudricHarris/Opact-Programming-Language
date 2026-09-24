#pragma once
#include "Symbol.hpp"
#include "../Types/StructType.hpp"

class StructSymbol : public Symbol {
	public:
		StructSymbol(std::string name, std::shared_ptr<StructType> structType)
			: Symbol(std::move(name), structType) {}

		SymbolKind getKind() const override { return SymbolKind::Class; }
};
