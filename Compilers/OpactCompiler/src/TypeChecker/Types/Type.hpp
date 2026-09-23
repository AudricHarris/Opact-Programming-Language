#pragma once
#include <string>
#include <memory>

enum class TypeKind {
	Primitive,
	Function,
	Struct
};

class Type {
	public:
		virtual ~Type() = default;
		virtual TypeKind getKind() const = 0;
		virtual std::string toString() const = 0;
		virtual bool equals(const Type* other) const = 0;
};
