#pragma once
#include <memory>
#include <string>
#include "Type.hpp"

class ArrayType : public Type {
public:
	ArrayType(std::shared_ptr<Type> elementType, size_t arrayLength)
		: elementType_(std::move(elementType)), arrayLength_(arrayLength) {
		// Automatically compute total size based on element size and length
		size_ = elementType_->getSize() * arrayLength_;
	}

	TypeKind getKind() const override { return TypeKind::Array; }

	std::shared_ptr<Type> getElementType() const { return elementType_; }
	size_t getArrayLength() const { return arrayLength_; }

	std::string toString() const override {
		return elementType_->toString() + "[" + std::to_string(arrayLength_) + "]";
	}

	bool equals(const Type* other) const override {
		if (!other || other->getKind() != TypeKind::Array) return false;
		auto arr = static_cast<const ArrayType*>(other);
		if (arrayLength_ != arr->arrayLength_) return false;
		if (!elementType_->equals(arr->elementType_.get())) return false;
		return true;
	}

private:
	std::shared_ptr<Type> elementType_;
	size_t arrayLength_;
};
