#pragma once
#include "Type.hpp"

enum class BuiltinType {
	Int,
	Float,
	Bool,
	Char,   // added: AST has Char literals
	Str,    // added: AST has Str literals
	Void
};

class PrimitiveType : public Type {
	public:
		explicit PrimitiveType(BuiltinType type) : type_(type) {
			switch (type_) {
				case BuiltinType::Bool:  size_ = 1; break;
				case BuiltinType::Char:  size_ = 1; break;
				case BuiltinType::Int:   size_ = 4; break;
				case BuiltinType::Float: size_ = 4; break;
				case BuiltinType::Str:   size_ = 8; break;
				case BuiltinType::Void:  size_ = 0; break;
			}
		}

		// this is for i16 i32 i64
		PrimitiveType(BuiltinType type, size_t customSize) : type_(type) {
			size_ = customSize;
		}

		TypeKind getKind() const override { return TypeKind::Primitive; }
		BuiltinType getBuiltinType() const { return type_; }

		std::string toString() const override {
			switch (type_) {
				case BuiltinType::Int:   return "int";
				case BuiltinType::Float: return "float";
				case BuiltinType::Bool:  return "bool";
				case BuiltinType::Char:  return "char";
				case BuiltinType::Str:   return "str";
				case BuiltinType::Void:  return "void";
			}
			return "unknown";
		}

		bool equals(const Type* other) const override {
			if (!other || other->getKind() != TypeKind::Primitive) return false;
			return type_ == static_cast<const PrimitiveType*>(other)->type_;
		}

	private:
		BuiltinType type_;
};
