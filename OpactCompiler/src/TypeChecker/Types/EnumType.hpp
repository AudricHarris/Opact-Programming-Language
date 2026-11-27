#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "StructType.hpp"
#include "Type.hpp"

struct EnumVariantInfo {
	std::string name;
	int discriminant = 0;
	std::vector<StructField> payloadFields; // Empty for unit variants like `Error`
};

class EnumType : public Type {
public:
	explicit EnumType(std::string name) : name_(std::move(name)) {}

	TypeKind getKind() const override { return TypeKind::Enum; }
	const std::string& getName() const { return name_; }

	void addVariant(std::string variantName, int discriminant, std::vector<StructField> payload = {}) {
		size_t idx = variants_.size();
		variants_.push_back(EnumVariantInfo{std::move(variantName), discriminant, std::move(payload)});
		indexByName_[variants_[idx].name] = idx;
	}

	const EnumVariantInfo* getVariant(const std::string& variantName) const {
		auto it = indexByName_.find(variantName);
		return it != indexByName_.end() ? &variants_[it->second] : nullptr;
	}

	const std::vector<EnumVariantInfo>& getVariants() const { return variants_; }

	bool hasAnyPayload() const {
		for (const auto& v : variants_) {
			if (!v.payloadFields.empty()) return true;
		}
		return false;
	}

	std::string toString() const override { return "enum " + name_; }

	bool equals(const Type* other) const override {
		if (!other || other->getKind() != TypeKind::Enum) return false;
		return name_ == static_cast<const EnumType*>(other)->name_;
	}

	void setSize(size_t size) { this->size_ = size; }

private:
	std::string name_;
	std::vector<EnumVariantInfo> variants_;
	std::unordered_map<std::string, size_t> indexByName_;
};
