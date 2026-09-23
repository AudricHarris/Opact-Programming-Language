#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "Type.hpp"

struct StructField {
	std::string name;
	std::shared_ptr<Type> type;
	size_t offset;
};

class StructType : public Type {
	public:
		explicit StructType(std::string name) : name_(std::move(name)) {}

		TypeKind getKind() const override { return TypeKind::Struct; }

		void addField(const std::string& fieldName, std::shared_ptr<Type> type, size_t offset) {
			fields_[fieldName] = {fieldName, type, offset};
		}

		const StructField* getField(const std::string& fieldName) const {
			auto it = fields_.find(fieldName);
			return it != fields_.end() ? &it->second : nullptr;
		}

		std::string toString() const override { return "struct " + name_; }

		bool equals(const Type* other) const override {
			if (other->getKind() != TypeKind::Struct) return false;
			return name_ == static_cast<const StructType*>(other)->name_;
		}

	private:
		std::string name_;
		std::unordered_map<std::string, StructField> fields_;
};
