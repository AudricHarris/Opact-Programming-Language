#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include "../Symbol/Symbol.hpp"

class Scope {
	public:
		explicit Scope(Scope* parent = nullptr) : parent_(parent) {}

		bool declareSymbol(const std::string& name, std::shared_ptr<Symbol> symbol) {
			if (symbols_.find(name) != symbols_.end()) {
				return false; // Symbol already declared in current scope
			}
			symbols_[name] = symbol;
			return true;
		}

		std::shared_ptr<Symbol> lookup(const std::string& name) {
			auto it = symbols_.find(name);
			if (it != symbols_.end()) {
				return it->second;
			}
			if (parent_) {
				return parent_->lookup(name);
			}
			return nullptr;
		}

		Scope* getParent() const { return parent_; }

	private:
		Scope* parent_;
		std::unordered_map<std::string, std::shared_ptr<Symbol>> symbols_;
};
