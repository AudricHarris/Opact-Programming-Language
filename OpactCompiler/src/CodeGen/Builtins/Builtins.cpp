//==========================================================================//
//  Builtins.cpp  -  builtin registry                                       //
//  registerBuiltins() lists every builtin module. One line per module.     //
//  registerBuiltin / registerExternal are the two registration primitives. //
//  See CodeGen/docs/ADDING_FEATURES.md for a step-by-step recipe.          //
//==========================================================================//
#include "../CodeGenerator.hpp"

void CodeGenerator::registerBuiltins() {
	registerFormatBuiltins();   // Printf, Println      -> Format.cpp
	registerRangeBuiltins();    // Range                -> Range.cpp
	registerRandomBuiltins();   // Random               -> Random.cpp
	registerReadBuiltins();     // Read                 -> Read.cpp
	registerLibcBuiltins();     // Exit, Puts, Abs      -> Libc.cpp
	registerMathBuiltins();     // Sqrt, Pow, Max, ...  -> Math.cpp   (example module)

	// >>> ADD A NEW MODULE HERE: registerYourBuiltins(); <<<
}

void CodeGenerator::registerBuiltin(const std::string& name, BuiltinHandler handler) {
	builtins[name] = std::move(handler);
}

void CodeGenerator::registerExternal(const std::string& userName, const std::string& cName,
		llvm::Type* ret, std::vector<llvm::Type*> params, bool variadic) {
	registerBuiltin(userName, [this, userName, cName, ret, params, variadic](CallExpr& call) -> llvm::Value* {
		bool badCount = variadic ? call.arguments.size() < params.size()
		                         : call.arguments.size() != params.size();
		if (badCount) {
			error(userName + ": expected " + std::to_string(params.size()) + " argument(s), got " +
					std::to_string(call.arguments.size()));
			return nullptr;
		}

		auto callee = module->getOrInsertFunction(cName, llvm::FunctionType::get(ret, params, variadic));

		std::vector<llvm::Value*> args;
		for (size_t i = 0; i < call.arguments.size(); ++i) {
			llvm::Value* v = emit(call.arguments[i].get());
			if (!v) return nullptr;
			v = (i < params.size()) ? coerce(v, params[i]) : promoteVarArg(v);
			if (!v) return nullptr;
			args.push_back(v);
		}
		if (ret->isVoidTy()) return builder.CreateCall(callee, args);
		return builder.CreateCall(callee, args, "calltmp");
	});
}
