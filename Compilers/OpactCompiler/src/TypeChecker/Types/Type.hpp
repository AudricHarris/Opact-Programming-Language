#pragma once
#include <cstddef>
#include <string>

enum class TypeKind {
	Primitive,
	Function,
	Struct,
	Reference,
	Array
};

class Type {
	public:
		virtual ~Type() = default;
		virtual TypeKind getKind() const = 0;
		virtual std::string toString() const = 0;
		virtual bool equals(const Type* other) const = 0;

		size_t getSize() const { return size_; }
	protected:
		size_t size_ = 0;
};
