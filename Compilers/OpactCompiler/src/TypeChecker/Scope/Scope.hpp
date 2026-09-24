#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include "../Symbol/Symbol.hpp"

class Scope {
	public:
		explicit Scope(Scope* parent = nullptr) : parent(parent) {}

		bool declareSymbol(const std::string& name, std::shared_ptr<Symbol> symbol) {
			if (symbols.find(name) != symbols.end()) {
				return false; // Symbol already declared in current scope
			}
			symbols[name] = symbol;
			return true;
		}

		std::shared_ptr<Symbol> lookup(const std::string& name) {
			auto it = symbols.find(name);
			if (it != symbols.end()) {
				return it->second;
			}
			if (parent) {
				return parent->lookup(name);
			}
			return nullptr;
		}

		Scope* getParent() const { return parent; }

	private:
		Scope* parent;
		std::unordered_map<std::string, std::shared_ptr<Symbol>> symbols;
};
