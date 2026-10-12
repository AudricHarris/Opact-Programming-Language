#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <iostream>
#include "../Symbol/Symbol.hpp"

class Scope {
	public:
		explicit Scope(Scope* parent = nullptr) : parent(parent) {}

		bool declareSymbol(const std::string& name, std::shared_ptr<Symbol> symbol) {
			if (symbols.find(name) != symbols.end()) {
				return false; // Symbol already declared in current scope[cite: 4]
			}
			symbols[name] = symbol;
			return true;
		}

		[[nodiscard]] std::shared_ptr<Symbol> lookup(const std::string& name) const {
			auto it = symbols.find(name);
			if (it != symbols.end()) {
				return it->second;
			}
			if (parent) {
				return parent->lookup(name);
			}
			return nullptr;
		}

		[[nodiscard]] Scope* getParent() const { return parent; }

		// Debugging printer for symbols in this scope
		void dumpSymbols() const {
			if (symbols.empty()) {
				std::cout << "  [Scope is empty]\n";
				return;
			}
			for (const auto& [name, symbol] : symbols) {
				std::cout << "  * Symbol Name: \"" << name << "\"";
				std::cout << "\n";
			}
		}

	private:
		Scope* parent;
		std::unordered_map<std::string, std::shared_ptr<Symbol>> symbols;
};
