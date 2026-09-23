#pragma once
#include "Type.hpp"

enum class BuiltinType {
	Int,
	Float,
	Bool,
	Void
};

class PrimitiveType : public Type {
	public:
		explicit PrimitiveType(BuiltinType type) : type_(type) {}

		TypeKind getKind() const override { return TypeKind::Primitive; }
		BuiltinType getBuiltinType() const { return type_; }

		std::string toString() const override {
			switch (type_) {
				case BuiltinType::Int:   return "int";
				case BuiltinType::Float: return "float";
				case BuiltinType::Bool:  return "bool";
				case BuiltinType::Void:  return "void";
			}
			return "unknown";
		}

		bool equals(const Type* other) const override {
			if (other->getKind() != TypeKind::Primitive) return false;
			return type_ == static_cast<const PrimitiveType*>(other)->type_;
		}

	private:
		BuiltinType type_;
};
