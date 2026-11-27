//==========================================================================//
//  Blocks.cpp  -  basic-block helpers                                      //
//                                                                          //
//  blockTerminated, branchIfOpen, startDeadBlock, createEntryAlloca.       //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

// Don't call BasicBlock::getTerminator() here: newer LLVM asserts on blocks
// that are still being built (empty / no terminator yet).
bool CodeGenerator::blockTerminated() {
	llvm::BasicBlock* bb = builder.GetInsertBlock();
	return bb && !bb->empty() && bb->back().isTerminator();
}

void CodeGenerator::branchIfOpen(llvm::BasicBlock* target) {
	if (!blockTerminated()) builder.CreateBr(target);
}

// After return/break/continue: anything emitted next goes into an unreachable block.
void CodeGenerator::startDeadBlock() {
	builder.SetInsertPoint(llvm::BasicBlock::Create(context, "dead", currentFn));
}

llvm::AllocaInst* CodeGenerator::createEntryAlloca(llvm::Function* fn, llvm::Type* ty, const std::string& name) {
	llvm::IRBuilder<> tmp(&fn->getEntryBlock(), fn->getEntryBlock().begin());
	return tmp.CreateAlloca(ty, nullptr, name);
}
