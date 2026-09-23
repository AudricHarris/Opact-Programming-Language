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
			Scope* parent = scopes_.empty() ? nullptr : scopes_.back().get();
			scopes_.push_back(std::make_unique<Scope>(parent));
		}

		void exitScope() {
			if (scopes_.size() > 1) {
				scopes_.pop_back();
			}
		}

		Scope* currentScope() const {
			return scopes_.back().get();
		}

	private:
		std::vector<std::unique_ptr<Scope>> scopes_;
};
