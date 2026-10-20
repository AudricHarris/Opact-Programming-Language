#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
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

		const std::string& getName() const { return name_; }

		void addField(const std::string& fieldName, std::shared_ptr<Type> type, size_t offset) {
			fields_[fieldName] = {fieldName, type, offset};
			orderedFields_.push_back({fieldName, type, offset}); // Track order for positional checks
		}

		const StructField* getField(const std::string& fieldName) const {
			auto it = fields_.find(fieldName);
			return it != fields_.end() ? &it->second : nullptr;
		}

		const std::unordered_map<std::string, StructField>& getFields() const { return fields_; }
		const std::vector<StructField>& getOrderedFields() const { return orderedFields_; }
		
		std::string toString() const override { return "struct " + name_; }

		bool equals(const Type* other) const override {
			if (!other || other->getKind() != TypeKind::Struct) return false;
			return name_ == static_cast<const StructType*>(other)->name_;
		}

		void setSize(int size) {
			this->size_ = size;
		}

	private:
		std::string name_;
		std::unordered_map<std::string, StructField> fields_;
		std::vector<StructField> orderedFields_; // Preserves declaration order
};
