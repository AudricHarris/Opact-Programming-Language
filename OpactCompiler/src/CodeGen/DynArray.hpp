#ifndef CODEGEN_DYNARRAY_HPP
#define CODEGEN_DYNARRAY_HPP

//==========================================================================//
//  DynArray.hpp  -  arrays with unknown ('?') dimensions                   //
//                                                                          //
//  `Tile[?][?]` can't be a fixed LLVM array, so it is stored as a small    //
//  descriptor struct  { ptr data, i64 dim0, i64 dim1, ... }  (one i64 per  //
//  dimension). The data lives on the heap (calloc) and is indexed          //
//  row-major:  a[i][j] -> data[i * dim1 + j].                              //
//                                                                          //
//  Used by: Core/Types.cpp, Core/LValues.cpp, Visitors/Arrays.cpp,         //
//           Visitors/ForLoop.cpp, Visitors/Structs.cpp                     //
//==========================================================================//

#include "../Parser/Ast.hpp"

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace codegen_detail {

struct DynArrayInfo {
	llvm::Type* elemTy; // element type (after all dimensions)
	size_t      rank;   // number of dimensions
};

// Registries (defined once in DynArray.cpp).
extern std::unordered_map<const llvm::Type*, DynArrayInfo>         g_dynArrays;
extern std::map<std::pair<llvm::Type*, size_t>, llvm::StructType*> g_dynTypes;

const DynArrayInfo* dynInfo(const llvm::Type* t);
bool hasUnknownDim(const TypeDesc& d);
llvm::StructType* getDynArrayType(llvm::LLVMContext& ctx, llvm::Type* elem, size_t rank);

// Type left after applying `n` indices (a descriptor consumes `rank` of them).
llvm::Type* peelIndexTy(llvm::Type* ty, size_t n);

// Address of element [i0][i1]... of a descriptor stored at `dynAddr`.
// `idx` holds exactly `rank` i64 indices.
template <class B>
llvm::Value* dynElemPtr(B& b, const DynArrayInfo& info, llvm::StructType* dynTy,
		llvm::Value* dynAddr, const std::vector<llvm::Value*>& idx) {
	llvm::Type* i64 = b.getInt64Ty();
	llvm::Value* data = b.CreateLoad(b.getPtrTy(), b.CreateStructGEP(dynTy, dynAddr, 0), "dyn_data");
	llvm::Value* linear = idx[0];
	for (size_t k = 1; k < info.rank; ++k) {
		llvm::Value* dim = b.CreateLoad(i64, b.CreateStructGEP(dynTy, dynAddr, k + 1), "dyn_dim");
		linear = b.CreateAdd(b.CreateMul(linear, dim), idx[k]);
	}
	return b.CreateInBoundsGEP(info.elemTy, data, { linear }, "dyn_elem_ptr");
}

} // namespace codegen_detail

#endif // CODEGEN_DYNARRAY_HPP
