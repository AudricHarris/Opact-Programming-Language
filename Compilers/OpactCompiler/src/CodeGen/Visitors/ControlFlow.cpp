//==========================================================================//
//  ControlFlow.cpp  -  blocks, if, while, loop, break, continue, return    //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(Block& node) {
	// At module level: register structs and function prototypes first so
	// declaration order does not matter.
	if (!currentFn) {
		for (auto& e : node.expressions) {
			if (auto* s = dynamic_cast<StructDeclExpr*>(e.get()); s && !structs.count(s->name)) {
				StructInfo info;
				info.type = llvm::StructType::create(context, s->name);
				structs[s->name] = std::move(info);
			}
		}
		for (auto& e : node.expressions) {
			if (auto* f = dynamic_cast<FunctionDeclExpr*>(e.get())) declareFunction(*f);
		}
	}

	pushScope();
	llvm::Value* last = nullptr;
	for (auto& e : node.expressions) {
		if (e) last = emit(e.get());
	}
	popScope();
	lastValue = last;
}

void CodeGenerator::visit(IfExpr& node) {
	llvm::Value* cond = toBool(emit(node.condition.get()));
	if (!cond || !node.ifBranch) return;

	auto* thenBB  = llvm::BasicBlock::Create(context, "then", currentFn);
	auto* elseBB  = (node.elseBranch && *node.elseBranch) ? llvm::BasicBlock::Create(context, "else", currentFn) : nullptr;
	auto* mergeBB = llvm::BasicBlock::Create(context, "ifcont", currentFn);

	builder.CreateCondBr(cond, thenBB, elseBB ? elseBB : mergeBB);

	builder.SetInsertPoint(thenBB);
	emit(node.ifBranch.get());
	branchIfOpen(mergeBB);

	if (elseBB) {
		builder.SetInsertPoint(elseBB);
		emit(node.elseBranch->get());
		branchIfOpen(mergeBB);
	}

	builder.SetInsertPoint(mergeBB);
	lastValue = nullptr;
}

void CodeGenerator::visit(WhileExpr& node) {
	if (!node.condition || !node.loopBranch) return;

	auto* condBB  = llvm::BasicBlock::Create(context, "whilecond",  currentFn);
	auto* loopBB  = llvm::BasicBlock::Create(context, "whileloop",  currentFn);
	auto* afterBB = llvm::BasicBlock::Create(context, "whileafter", currentFn);

	builder.CreateBr(condBB);
	builder.SetInsertPoint(condBB);
	llvm::Value* cond = toBool(emit(node.condition.get()));
	if (!cond) return;
	builder.CreateCondBr(cond, loopBB, afterBB);

	builder.SetInsertPoint(loopBB);
	loopStack.push_back({ afterBB, condBB });
	emit(node.loopBranch.get());
	loopStack.pop_back();
	branchIfOpen(condBB);

	builder.SetInsertPoint(afterBB);
	lastValue = nullptr;
}

void CodeGenerator::visit(LoopExpr& node) {
	if (!node.loopBranch) return;

	auto* bodyBB  = llvm::BasicBlock::Create(context, "loop",      currentFn);
	auto* afterBB = llvm::BasicBlock::Create(context, "loopafter", currentFn);

	builder.CreateBr(bodyBB);
	builder.SetInsertPoint(bodyBB);
	loopStack.push_back({ afterBB, bodyBB });
	emit(node.loopBranch.get());
	loopStack.pop_back();
	branchIfOpen(bodyBB);

	builder.SetInsertPoint(afterBB);
	lastValue = nullptr;
}

void CodeGenerator::visit(BreakExpr&) {
	if (loopStack.empty()) { error("'break' outside of a loop"); return; }
	builder.CreateBr(loopStack.back().breakBB);
	startDeadBlock();
}

void CodeGenerator::visit(ContinueExpr&) {
	if (loopStack.empty()) { error("'continue' outside of a loop"); return; }
	builder.CreateBr(loopStack.back().continueBB);
	startDeadBlock();
}

void CodeGenerator::visit(ReturnExpr& node) {
	if (!currentFn) { error("'return' outside of a function"); return; }

	// Unsigned return types can never return a negative constant.
	auto sg = fnSigns.find(currentFn->getName().str());
	llvm::Value* v = nullptr;
	if (node.value && *node.value) {
		bool u = sg != fnSigns.end() && sg->second.retUnsigned &&
			currentFn->getReturnType()->isIntegerTy();
		v = u ? emitUnsignedValue(node.value->get(), currentFn->getReturnType(), "return value")
			: emit(node.value->get());
	}
	llvm::Type* rt = currentFn->getReturnType();

	if (!rt->isVoidTy()) {
		v = v ? coerce(v, rt) : llvm::Constant::getNullValue(rt);
		if (!v) return;
	}

	// 2) Emit cleanups for every open scope (innermost first) WITHOUT popping
	//    the compile-time stack. The returned pointer is excluded from freeing.
	llvm::Value* keep = (v && v->getType()->isPointerTy()) ? v : nullptr;
	for (auto it = scopeCleanups.rbegin(); it != scopeCleanups.rend(); ++it) {
		emitCleanups(*it, keep);
	}

	// 3) Return.
	if (rt->isVoidTy()) builder.CreateRetVoid();
	else builder.CreateRet(v);

	startDeadBlock();
	lastValue = nullptr;
}
