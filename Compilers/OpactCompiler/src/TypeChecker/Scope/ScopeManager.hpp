#pragma once
#include <vector>
#include <memory>
#include <iostream>
#include "Scope.hpp"
#include "../Symbol/Symbol.hpp"

class ScopeManager {
	public:
		ScopeManager() {
			enterScope(); // Global scope[cite: 3]
		}

		void enterScope() {
			Scope* parent = scopes.empty() ? nullptr : scopes.back().get();
			scopes.push_back(std::make_unique<Scope>(parent));
		}

		void exitScope() {
			if (scopes.size() > 1) {
				scopes.pop_back();
			}
		}

		[[nodiscard]] Scope* currentScope() const {
			return scopes.back().get();
		}

		// Core declaration method
		bool declare(const std::string& name, std::shared_ptr<Symbol> symbol) {
			return currentScope()->declareSymbol(name, symbol);
		}

		// Lookup wrapper
		[[nodiscard]] std::shared_ptr<Symbol> lookup(const std::string& name) const {
			return currentScope()->lookup(name);
		}

		// --- Scope Debugging Utility ---
		void dump() const {
			std::cout << "SCOPE MANAGER DUMP\n";
			std::cout << "Total Active Scope Depth: " << scopes.size() << "\n\n";

			for (size_t i = 0; i < scopes.size(); ++i) {
				std::cout << "Scope Index [" << i << "] (Address: " << scopes[i].get() 
					<< ", Parent: " << scopes[i]->getParent() << ")\n";
				scopes[i]->dumpSymbols();
			}
			std::cout << "\n\n";
		}

	private:
		std::vector<std::unique_ptr<Scope>> scopes;
};
