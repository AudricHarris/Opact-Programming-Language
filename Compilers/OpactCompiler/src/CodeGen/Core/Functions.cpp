//==========================================================================//
//  Functions.cpp  -  function declaration helpers                          //
//                                                                          //
//  declareFunction, declareImportedFunctions, emitDefaultReturn.           //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

llvm::Function* CodeGenerator::declareFunction(FunctionDeclExpr& node) {
	std::string name = (node.name == "Main") ? "main" : node.name;
	if (auto* existing = module->getFunction(name)) return existing;

	// main must return int even if the source omitted the return type
	llvm::Type* ret = (name == "main") ? builder.getInt32Ty() : mapType(node.returnType);
	if (!ret) return nullptr;

	std::vector<llvm::Type*> params;
	for (const auto& p : node.params) {
		llvm::Type* pt = mapType(p.type);
		if (!pt || pt->isVoidTy()) { error("Invalid type for parameter '" + p.name + "'"); return nullptr; }
		params.push_back(pt);
	}

	// Remember which parameters / return value are unsigned so calls and
	// returns can reject negative constants.
	FnSign sign;
	for (const auto& p : node.params)
		sign.paramUnsigned.push_back(!p.type.isReference && p.type.isUnsignedInteger());
	sign.retUnsigned = !node.returnType.isReference && node.returnType.isUnsignedInteger();
	fnSigns[name] = std::move(sign);

	// Only `public` functions (and main) are visible to other modules;
	// everything else gets internal linkage so it cannot be linked from outside.
	auto linkage = (name == "main" || node.isPublic) ? llvm::Function::ExternalLinkage
		: llvm::Function::InternalLinkage;
	auto* fnType = llvm::FunctionType::get(ret, params, false);
	return llvm::Function::Create(fnType, linkage, name, module.get());
}

void CodeGenerator::declareImportedFunctions(Expression* moduleRoot) {
	auto* block = dynamic_cast<Block*>(moduleRoot);
	if (!block) return;
	for (auto& e : block->expressions) {
		auto* f = dynamic_cast<FunctionDeclExpr*>(e.get());
		if (f && f->isPublic) declareFunction(*f); // declaration only, body lives in the other module
	}
}

void CodeGenerator::emitDefaultReturn() {
	if (blockTerminated()) return;
	llvm::Type* rt = currentFn->getReturnType();
	if (rt->isVoidTy()) builder.CreateRetVoid();
	else builder.CreateRet(llvm::Constant::getNullValue(rt));
}
