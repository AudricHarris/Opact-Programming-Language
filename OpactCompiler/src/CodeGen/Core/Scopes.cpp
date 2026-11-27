//==========================================================================//
//  Scopes.cpp  -  scopes, heap slots and cleanups                          //
//                                                                          //
//  Ownership of heap slots: createHeapSlot, registerCleanup, emitCleanups, //
//  popScope, transferHeapSlot.                                             //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

// A pointer-sized stack slot in the entry block, initialised to null right
// there. Because the null store sits at function entry, every path through the
// function sees a valid (null or real) pointer when the cleanup code loads it.
llvm::AllocaInst* CodeGenerator::createHeapSlot(const std::string& name) {
	llvm::BasicBlock& entry = currentFn->getEntryBlock();
	llvm::IRBuilder<> tmp(&entry, entry.begin());
	llvm::AllocaInst* slot = tmp.CreateAlloca(builder.getPtrTy(), nullptr, name);
	tmp.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), slot);
	return slot;
}

// Emits `if (*slot != null && *slot != keep) { free(*slot); *slot = null; }`
// for every slot. `keep` (may be nullptr) is a pointer that must survive, e.g.
// the string being returned from the function.
void CodeGenerator::emitCleanups(const std::vector<llvm::Value*>& slots, llvm::Value* keep) {
	if (slots.empty() || blockTerminated()) return;

	auto freeFn = module->getOrInsertFunction(
			"free", llvm::FunctionType::get(builder.getVoidTy(), { builder.getPtrTy() }, false));
	llvm::Value* nullPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());

	// Newest allocation first.
	for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
		llvm::Value* slot = *it;
		if (!slot) continue;

		llvm::Value* heapPtr = builder.CreateLoad(builder.getPtrTy(), slot, "heap_ptr");

		llvm::Value* shouldFree = builder.CreateIsNotNull(heapPtr, "is_not_null");
		if (keep && keep->getType()->isPointerTy()) {
			llvm::Value* notKept = builder.CreateICmpNE(heapPtr, keep, "not_kept");
			shouldFree = builder.CreateAnd(shouldFree, notKept, "should_free");
		}

		llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(context, "do_free", currentFn);
		llvm::BasicBlock* contBlock = llvm::BasicBlock::Create(context, "cont_free", currentFn);
		builder.CreateCondBr(shouldFree, freeBlock, contBlock);

		builder.SetInsertPoint(freeBlock);
		builder.CreateCall(freeFn, { heapPtr });
		builder.CreateStore(nullPtr, slot); // avoid double free if the slot is reused
		builder.CreateBr(contBlock);

		builder.SetInsertPoint(contBlock);
	}
}

void CodeGenerator::popScope() {
	if (scopeCleanups.empty()) return;

	auto currentScope = std::move(scopeCleanups.back());
	scopeCleanups.pop_back();

	emitCleanups(currentScope, nullptr);
}

void CodeGenerator::registerCleanup(llvm::Value* slot) {
	if (!scopeCleanups.empty() && slot) {
		scopeCleanups.back().push_back(slot);
	}
}

void CodeGenerator::transferHeapSlot(VarInfo* var, llvm::Value* slot) {
	if (!slot) return;

	for (auto& cleanupList : scopeCleanups) {
		cleanupList.erase(
			std::remove(cleanupList.begin(), cleanupList.end(), slot),
			cleanupList.end()
		);
	}

	if (!var) {
		// Not assigned to a tracked local variable: keep it in current scope if any.
		if (!scopeCleanups.empty()) {
			scopeCleanups.back().push_back(slot);
		}
		return;
	}

	// If `var` previously owned a different heap allocation, free it before replacing.
	if (var->ownedSlot && var->ownedSlot != slot) {
		llvm::Value* prevPtr = builder.CreateLoad(builder.getPtrTy(), var->ownedSlot, "prev_heap_ptr");
		llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(context, "free_prev_heap", currentFn);
		llvm::BasicBlock* contBB = llvm::BasicBlock::Create(context, "cont_prev_heap", currentFn);
		llvm::Value* nonNull = builder.CreateICmpNE(prevPtr, llvm::ConstantPointerNull::get(builder.getPtrTy()));
		builder.CreateCondBr(nonNull, freeBB, contBB);

		builder.SetInsertPoint(freeBB);
		auto freeFn = module->getOrInsertFunction(
			"free",
			llvm::FunctionType::get(builder.getVoidTy(), { builder.getPtrTy() }, false)
		);
		builder.CreateCall(freeFn, { prevPtr });
		builder.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), var->ownedSlot);
		builder.CreateBr(contBB);

		builder.SetInsertPoint(contBB);
	}

	var->ownedSlot = slot;

	if (!scopeCleanups.empty()) {
		auto& targetScope = (scopeCleanups.size() > 1) ? scopeCleanups[1] : scopeCleanups.back();
		targetScope.push_back(slot);
	}
}
