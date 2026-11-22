//==========================================================================//
//  Calls.cpp  -  visit(CallExpr)                                           //
//                                                                          //
//  User functions first, then builtins (see Builtins/).                    //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

void CodeGenerator::visit(CallExpr& node) {
	lastValue = nullptr;
	auto* id = dynamic_cast<IdentifierExpr*>(node.callee.get());
	if (!id) { error("Only direct calls by name are supported"); return; }

	// 1) user-defined function (user code wins over builtins)
	llvm::Function* callee = module->getFunction(id->name == "Main" ? "main" : id->name);
	if (callee) {
		if (callee->arg_size() != node.arguments.size()) {
			error("Function '" + id->name + "' expects " + std::to_string(callee->arg_size()) +
					" argument(s), got " + std::to_string(node.arguments.size()));
			return;
		}
		auto sg = fnSigns.find(callee->getName().str());

		std::vector<llvm::Value*> args;
		size_t i = 0;
		for (auto& a : node.arguments) {
			size_t argIdx = i;
			llvm::Type* pt = callee->getFunctionType()->getParamType(i++);

			bool unsignedParam = sg != fnSigns.end() && argIdx < sg->second.paramUnsigned.size() &&
				sg->second.paramUnsigned[argIdx] && pt->isIntegerTy();
			llvm::Value* v = unsignedParam
				? emitUnsignedValue(a.get(), pt, "parameter " + std::to_string(argIdx + 1))
				: (pt->isPointerTy() ? emitRef(a.get()) : emit(a.get()));
			if (!v) return;
			// struct passed by value but we hold its address (temporary / reference)
			if (pt->isStructTy() && v->getType()->isPointerTy())
				v = builder.CreateLoad(pt, v, "structarg");
			v = coerce(v, pt);
			if (!v) return;
			args.push_back(v);
		}
		lastValue = callee->getReturnType()->isVoidTy() ? builder.CreateCall(callee, args)
			: builder.CreateCall(callee, args, "calltmp");
		return;
	}

	// 2) builtin (see Builtins.cpp)
	auto b = builtins.find(id->name);
	if (b != builtins.end()) {
		lastValue = b->second(node);
		return;
	}

	error("Unknown function '" + id->name + "'");
}
