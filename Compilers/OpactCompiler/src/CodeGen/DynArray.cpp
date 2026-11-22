//==========================================================================//
//  DynArray.cpp  -  see DynArray.hpp                                       //
//==========================================================================//
#include "DynArray.hpp"

namespace codegen_detail {

std::unordered_map<const llvm::Type*, DynArrayInfo>         g_dynArrays;
std::map<std::pair<llvm::Type*, size_t>, llvm::StructType*> g_dynTypes;

const DynArrayInfo* dynInfo(const llvm::Type* t) {
	auto it = g_dynArrays.find(t);
	return it == g_dynArrays.end() ? nullptr : &it->second;
}

bool hasUnknownDim(const TypeDesc& d) {
	for (int x : d.dim) if (x < 0) return true;
	return false;
}

llvm::StructType* getDynArrayType(llvm::LLVMContext& ctx, llvm::Type* elem, size_t rank) {
	auto key = std::make_pair(elem, rank);
	auto it = g_dynTypes.find(key);
	if (it != g_dynTypes.end()) return it->second;

	std::vector<llvm::Type*> fields;
	fields.push_back(llvm::PointerType::getUnqual(ctx));
	for (size_t i = 0; i < rank; ++i) fields.push_back(llvm::Type::getInt64Ty(ctx));

	auto* st = llvm::StructType::create(ctx, fields, "dynarray");
	g_dynTypes[key] = st;
	g_dynArrays[st] = { elem, rank };
	return st;
}

llvm::Type* peelIndexTy(llvm::Type* ty, size_t n) {
	size_t i = 0;
	while (ty && i < n) {
		if (const DynArrayInfo* d = dynInfo(ty)) {
			ty = d->elemTy;
			i += d->rank;
		} else {
			if (ty->isArrayTy()) ty = llvm::cast<llvm::ArrayType>(ty)->getElementType();
			++i;
		}
	}
	return ty;
}

} // namespace codegen_detail
