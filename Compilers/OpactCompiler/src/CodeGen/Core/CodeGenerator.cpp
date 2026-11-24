//==========================================================================//
//  CodeGenerator.cpp  -  constructor and core helpers                      //
//                                                                          //
//  Constructor, error reporting, emit() / emitRef() entry points, variable lookup.//
//  Everything else lives in a topic file (see CodeGen/docs/ADDING_FEATURES.md).//
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

CodeGenerator::CodeGenerator(const std::string& moduleName, llvm::LLVMContext& ctx)
	: context(ctx), builder(ctx), module(std::make_unique<llvm::Module>(moduleName, ctx)) {
	scopes.emplace_back(); // global scope
	registerBuiltins();
}

void CodeGenerator::error(const std::string& msg) {
	++errorCount;
	std::cerr << "Codegen Error: " << msg << "\n";
}

// Evaluate an expression and return its value (nullptr if none / error).
llvm::Value* CodeGenerator::emit(Expression* e) {
	if (!e) return nullptr;
	lastValue = nullptr;
	e->accept(*this);
	return lastValue;
}

// Evaluates `e` where a *reference* (an address) is wanted, not a value.
// A reference variable to a primitive is normally auto-dereferenced when read
// (see visit(IdentifierExpr)); here we hand out the stored pointer instead, so
// references can be forwarded to other functions / reference variables.
llvm::Value* CodeGenerator::emitRef(Expression* e) {
	if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
		VarInfo* var = lookupVar(id->name);
		if (var && var->pointee)
			return builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_ref");
	}
	return emit(e);
}

CodeGenerator::VarInfo* CodeGenerator::lookupVar(const std::string& name) {
	for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
		auto f = it->find(name);
		if (f != it->end()) return &f->second;
	}
	return nullptr;
}

