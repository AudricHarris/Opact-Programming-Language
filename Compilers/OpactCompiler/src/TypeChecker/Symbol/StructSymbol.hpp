#pragma once
#include "Symbol.hpp"
#include "../Types/StructType.hpp"
#include <iostream>

class StructSymbol : public Symbol {
	public:
		StructSymbol(std::string name, std::shared_ptr<StructType> structType)
			: Symbol(std::move(name), structType) {}

		SymbolKind getKind() const override {std::cout << "Struct\n"; return SymbolKind::Struct; }
};
