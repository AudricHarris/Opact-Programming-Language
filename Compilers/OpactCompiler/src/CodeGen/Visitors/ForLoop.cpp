//==========================================================================//
//  ForLoop.cpp  -  visit(ForExpr)                                          //
//                                                                          //
//  Ranges, fixed arrays and dynamic arrays.                                //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(ForExpr& node) {
	if (!node.iterable || !node.body || !currentFn) {
		lastValue = nullptr;
		return;
	}

	pushScope();
	scopes.emplace_back();

	llvm::Value* iterVal = emit(node.iterable.get());
	if (!iterVal) {
		scopes.pop_back();
		popScope();
		lastValue = nullptr;
		return;
	}

	llvm::Type* iterTy = iterVal->getType();

	// Array literals / allocations, indexed sub-arrays and builtins like Range()
	// come back as the *address* of a temporary. Recover what it points at.
	llvm::Value* iterPtr = nullptr;
	if (iterTy->isPointerTy()) {
		if (auto* ai = llvm::dyn_cast<llvm::AllocaInst>(iterVal)) {
			iterPtr = ai;
			iterTy  = ai->getAllocatedType();
		} else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(iterVal)) {
			iterPtr = gep;
			iterTy  = gep->getResultElementType();
		}
	}

	// Determine loop bounds & element type.
	llvm::Value* startVal = nullptr;
	llvm::Value* endVal   = nullptr;
	llvm::Type*  elemTy   = nullptr;
	llvm::Value* dataBase = nullptr; // Non-null when indexing into an array/buffer.
	bool         flatData = false;   // dataBase is a plain element buffer (GEP bufElemTy, data, i)
	llvm::Type*  bufElemTy = nullptr; // element type of that buffer
									  // Row mode ('?' array of rank > 1 looped with a Tile[?]-style variable):
									  // each step yields a rank-1 descriptor { &row, remaining dims... }.
	llvm::StructType*          sliceTy = nullptr;
	llvm::Value*               rowStride = nullptr;
	std::vector<llvm::Value*>  rowDims;

	if (const DynArrayInfo* dyn = dynInfo(iterTy)) {
		// '?' array: visit every element in row-major order.
		size_t declDims = 0;
		if (auto* d = dynamic_cast<VarDeclExpr*>(node.itemDecl.get())) declDims = d->type.dim.size();

		llvm::Value* dynVal = iterPtr ? builder.CreateLoad(iterTy, iterPtr, "fordyn") : iterVal;
		dataBase = builder.CreateExtractValue(dynVal, 0, "fordata");
		std::vector<llvm::Value*> dims;
		for (size_t k = 0; k < dyn->rank; ++k)
			dims.push_back(builder.CreateExtractValue(dynVal, static_cast<unsigned>(k + 1), "fordim"));

		startVal  = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
		bufElemTy = dyn->elemTy;
		flatData  = true;

		if (dyn->rank > 1 && declDims == dyn->rank - 1) {
			// Rows: `for (Tile[?] row : grid)`. Step over the first dimension.
			sliceTy   = getDynArrayType(context, dyn->elemTy, dyn->rank - 1);
			endVal    = dims[0];
			rowStride = dims[1];
			for (size_t k = 2; k < dyn->rank; ++k) rowStride = builder.CreateMul(rowStride, dims[k], "forstride");
			rowDims.assign(dims.begin() + 1, dims.end());
			elemTy    = sliceTy;
		} else if (dyn->rank == 1 || declDims == 0) {
			// Single elements, row-major over every dimension.
			llvm::Value* total = dims[0];
			for (size_t k = 1; k < dyn->rank; ++k) total = builder.CreateMul(total, dims[k], "fortotal");
			endVal = total;
			elemTy = dyn->elemTy;
		} else {
			error("Loop variable has " + std::to_string(declDims) + " dimension(s), but looping over an array with " +
					std::to_string(dyn->rank) + " unknown dimensions yields rows with " +
					std::to_string(dyn->rank - 1) + " (or single elements with 0)");
			scopes.pop_back();
			popScope();
			lastValue = nullptr;
			return;
		}
	} else if (iterTy->isArrayTy()) {
		uint64_t count = iterTy->getArrayNumElements();
		elemTy = iterTy->getArrayElementType();

		if (iterPtr) {
			dataBase = iterPtr; // iterate in place
		} else {
			// Spill array value to an alloca so we can GEP element [0, idx].
			auto* arrAlloca = createEntryAlloca(currentFn, iterTy, "forarr");
			builder.CreateStore(iterVal, arrAlloca);
			dataBase = arrAlloca;
		}

		llvm::Type* idxTy = llvm::Type::getInt64Ty(context);
		startVal = llvm::ConstantInt::get(idxTy, 0);
		endVal   = llvm::ConstantInt::get(idxTy, count);
	} else if (auto* structTy = llvm::dyn_cast<llvm::StructType>(iterTy);
			structTy && structTy->getName().starts_with("Range")) {
		llvm::Value* rangeAlloca = iterVal->getType()->isPointerTy() ? iterVal : nullptr;
		if (!rangeAlloca) {
			rangeAlloca = createEntryAlloca(currentFn, structTy, "forrange");
			builder.CreateStore(iterVal, rangeAlloca);
		}
		llvm::Type* boundTy = structTy->getElementType(0);
		startVal = builder.CreateLoad(boundTy, builder.CreateStructGEP(structTy, rangeAlloca, 0), "rangestart");
		endVal   = builder.CreateLoad(boundTy, builder.CreateStructGEP(structTy, rangeAlloca, 1), "rangeend");
		elemTy   = boundTy;
	} else {
		error("Unsupported iterable type in for loop");
		scopes.pop_back();
		popScope();
		lastValue = nullptr;
		return;
	}

	llvm::Type* idxTy = startVal->getType();
	auto* idxAlloca = createEntryAlloca(currentFn, idxTy, "foridx");
	builder.CreateStore(startVal, idxAlloca);

	llvm::Value* elemAddr   = nullptr;
	llvm::Type*  elemSlotTy = nullptr;

	if (auto* decl = dynamic_cast<VarDeclExpr*>(node.itemDecl.get())) {
		elemSlotTy = elemTy ? elemTy : mapType(decl->type);
		auto* allocaInst = createEntryAlloca(currentFn, elemSlotTy, decl->name.getWord());
		elemAddr = allocaInst;

		VarInfo info;
		info.addr = allocaInst;
		info.type = elemSlotTy;
		info.isUnsigned = decl->type.isReference ? false : (decl->type.type == DataType::U8 ||
				decl->type.type == DataType::U16 || decl->type.type == DataType::U32 ||
				decl->type.type == DataType::U64);
		if (auto* st = llvm::dyn_cast<llvm::StructType>(elemSlotTy)) {
			info.structType = st;
		}
		scopes.back()[decl->name.getWord()] = info;
	} else if (auto* ident = dynamic_cast<IdentifierExpr*>(node.itemDecl.get())) {
		VarInfo* existing = lookupVar(ident->name);
		if (!existing) {
			error("Undefined variable '" + ident->name + "'");
			scopes.pop_back();
			popScope();
			lastValue = nullptr;
			return;
		}
		elemAddr = existing->addr;
		elemSlotTy = existing->type;
	} else if (node.itemDecl) {
		error("Invalid target in for loop");
		scopes.pop_back();
		popScope();
		lastValue = nullptr;
		return;
	}

	auto* condBB  = llvm::BasicBlock::Create(context, "forcond",  currentFn);
	auto* loopBB  = llvm::BasicBlock::Create(context, "forloop",  currentFn);
	auto* stepBB  = llvm::BasicBlock::Create(context, "forstep",  currentFn);
	auto* afterBB = llvm::BasicBlock::Create(context, "forafter", currentFn);

	builder.CreateBr(condBB);

	builder.SetInsertPoint(condBB);
	llvm::Value* curIdx = builder.CreateLoad(idxTy, idxAlloca, "idx");
	llvm::Value* cmp = idxTy->isFloatingPointTy()
		? builder.CreateFCmpOLT(curIdx, endVal, "forcmp")
		: builder.CreateICmpSLT(curIdx, endVal, "forcmp");
	builder.CreateCondBr(cmp, loopBB, afterBB);

	builder.SetInsertPoint(loopBB);
	if (elemAddr) {
		llvm::Value* toStore = nullptr;
		if (dataBase) {
			llvm::Value* elemPtr = nullptr;
			if (flatData && sliceTy) {
				// Build the row descriptor { &data[idx * stride], dims... }.
				llvm::Value* off = builder.CreateMul(curIdx, rowStride, "forrowoff");
				llvm::Value* rowPtr = builder.CreateInBoundsGEP(bufElemTy, dataBase, { off }, "forrowptr");
				llvm::Value* row = llvm::PoisonValue::get(sliceTy);
				row = builder.CreateInsertValue(row, rowPtr, 0);
				for (size_t k = 0; k < rowDims.size(); ++k)
					row = builder.CreateInsertValue(row, rowDims[k], static_cast<unsigned>(k + 1));
				toStore = row;
			} else if (flatData) {
				elemPtr = builder.CreateInBoundsGEP(bufElemTy, dataBase, { curIdx }, "forelemptr");
			} else {
				llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0);
				elemPtr = builder.CreateInBoundsGEP(iterTy, dataBase, { zero, curIdx }, "forelemptr");
			}
			if (!toStore) toStore = builder.CreateLoad(elemTy, elemPtr, "forelem");
		} else {
			toStore = curIdx;
		}

		if (elemSlotTy && toStore->getType() != elemSlotTy) {
			toStore = coerce(toStore, elemSlotTy);
		}
		builder.CreateStore(toStore, elemAddr);
	}

	loopStack.push_back({ afterBB, stepBB });
	emit(node.body.get());
	loopStack.pop_back();
	branchIfOpen(stepBB);

	builder.SetInsertPoint(stepBB);
	llvm::Value* idxForStep = builder.CreateLoad(idxTy, idxAlloca, "idxstep");
	llvm::Value* one = llvm::ConstantInt::get(idxTy, 1);
	llvm::Value* nextIdx = builder.CreateAdd(idxForStep, one, "idxnext");
	builder.CreateStore(nextIdx, idxAlloca);
	builder.CreateBr(condBB);

	builder.SetInsertPoint(afterBB);
	scopes.pop_back();
	popScope();
	lastValue = nullptr;
}
