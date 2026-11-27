#pragma once
#include "Symbol.hpp"
#include "../Types/EnumType.hpp"
#include <memory>
#include <string>
#include <utility>

class EnumSymbol : public Symbol {
public:
	EnumSymbol(std::string name, std::shared_ptr<EnumType> enumType)
		: Symbol(std::move(name), std::move(enumType)) {}

	SymbolKind getKind() const override { return SymbolKind::Enum; }
};
