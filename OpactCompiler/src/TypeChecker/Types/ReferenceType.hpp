#pragma once
#include "Type.hpp"
#include <memory>

class ReferenceType : public Type {
	public:
		ReferenceType(std::shared_ptr<Type> innerType, bool isMutable)
			: innerType_(std::move(innerType)), isMutable_(isMutable) {
			size_ = sizeof(void*);
		}

		TypeKind getKind() const override { return TypeKind::Reference; }

		std::shared_ptr<Type> getInnerType() const { return innerType_; }
		bool isMutable() const { return isMutable_; }

		std::string toString() const override {
			return (isMutable_ ? "&mut " : "&") + innerType_->toString();
		}

		// Strict equality (remains strict)
		bool equals(const Type* other) const override {
			if (!other || other->getKind() != TypeKind::Reference) return false;
			auto* oRef = static_cast<const ReferenceType*>(other);
			return isMutable_ == oRef->isMutable_ && innerType_->equals(oRef->innerType_.get());
		}

		// Allows &mut T to be compatible with &T, but not vice versa
		bool isCompatibleWith(const Type* other) const {
			if (!other) return false;
			if (equals(other)) return true;

			if (other->getKind() == TypeKind::Reference) {
				auto* oRef = static_cast<const ReferenceType*>(other);
				if (isMutable_ && !oRef->isMutable_) {
					return innerType_->equals(oRef->innerType_.get());
				}
			}
			return false;
		}

	private:
		std::shared_ptr<Type> innerType_;
		bool isMutable_;
};
