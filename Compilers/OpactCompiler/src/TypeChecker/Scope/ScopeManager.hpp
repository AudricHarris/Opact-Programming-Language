#pragma once
#include <vector>
#include <memory>
#include "Scope.hpp"

class ScopeManager {
	public:
		ScopeManager() {
			enterScope(); // Global scope
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

		Scope* currentScope() const {
			return scopes.back().get();
		}

	private:
		std::vector<std::unique_ptr<Scope>> scopes;
};
