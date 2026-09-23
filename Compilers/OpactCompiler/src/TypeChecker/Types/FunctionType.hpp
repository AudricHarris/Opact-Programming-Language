#pragma once
#include <vector>
#include "Type.hpp"

class FunctionType : public Type {
	public:
		FunctionType(std::shared_ptr<Type> returnType, std::vector<std::shared_ptr<Type>> paramTypes)
			: returnType_(returnType), paramTypes_(std::move(paramTypes)) {}

		TypeKind getKind() const override { return TypeKind::Function; }

		std::shared_ptr<Type> getReturnType() const { return returnType_; }
		const std::vector<std::shared_ptr<Type>>& getParamTypes() const { return paramTypes_; }

		std::string toString() const override {
			std::string res = "(";
			for (size_t i = 0; i < paramTypes_.size(); ++i) {
				res += paramTypes_[i]->toString();
				if (i + 1 < paramTypes_.size()) res += ", ";
			}
			res += ") -> " + returnType_->toString();
			return res;
		}

		bool equals(const Type* other) const override {
			if (other->getKind() != TypeKind::Function) return false;
			auto fn = static_cast<const FunctionType*>(other);
			if (!returnType_->equals(fn->returnType_.get())) return false;
			if (paramTypes_.size() != fn->paramTypes_.size()) return false;
			for (size_t i = 0; i < paramTypes_.size(); ++i) {
				if (!paramTypes_[i]->equals(fn->paramTypes_[i].get())) return false;
			}
			return true;
		}

	private:
		std::shared_ptr<Type> returnType_;
		std::vector<std::shared_ptr<Type>> paramTypes_;
};
