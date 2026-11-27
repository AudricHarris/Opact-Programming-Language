#pragma once
#include "Symbol.hpp"

class VarSymbol : public Symbol {
	public:
		VarSymbol(std::string name, std::shared_ptr<Type> type, bool isInitialized = false)
			: Symbol(std::move(name), type), isInitialized_(isInitialized) {}

		SymbolKind getKind() const override { return SymbolKind::Variable; }

		bool isInitialized() const { return isInitialized_; }
		void markInitialized() { isInitialized_ = true; }

	private:
		bool isInitialized_;
};
