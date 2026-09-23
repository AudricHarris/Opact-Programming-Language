#pragma once
#include <string>
#include <memory>
#include "../Types/Type.hpp"

enum class SymbolKind {
	Variable,
	Function,
	Class
};

class Symbol {
	public:
		Symbol(std::string name, std::shared_ptr<Type> type) 
			: name_(std::move(name)), type_(type) {}
		virtual ~Symbol() = default;

		virtual SymbolKind getKind() const = 0;

		const std::string& getName() const { return name_; }
		std::shared_ptr<Type> getType() const { return type_; }

	protected:
		std::string name_;
		std::shared_ptr<Type> type_;
};
