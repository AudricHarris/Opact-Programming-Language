//==========================================================================//
//  Math.cpp  -  EXAMPLE builtin module: Sqrt, Pow, Min, Max                //
//                                                                          //
//  Copy this file when you want to add your own group of builtins.         //
//  It shows the two ways to define one:                                    //
//                                                                          //
//    A) wrap a C function            -> registerExternal(...)              //
//    B) emit your own LLVM IR        -> registerBuiltin(name, lambda)      //
//==========================================================================//
#include "../CodeGenerator.hpp"

void CodeGenerator::registerMathBuiltins() {
	llvm::Type* f64 = builder.getDoubleTy();

	// ---- A) C wrappers (the compiler declares sqrt/pow lazily, on first use).
	//         Arguments are coerced to the declared types, so Sqrt(16) works.
	//         NOTE: needs libm at link time (-lm) on some platforms.
	registerExternal("Sqrt", "sqrt", f64, { f64 });
	registerExternal("Pow",  "pow",  f64, { f64, f64 });

	// ---- B) Custom IR: Min(a, b) / Max(a, b) for ints or floats.
	//         A handler gets the CallExpr, emits IR with `builder`, and returns the
	//         resulting llvm::Value* (or nullptr after calling error(...)).
	auto defineMinMax = [this](const std::string& name, bool wantMax) {
		registerBuiltin(name, [this, name, wantMax](CallExpr& call) -> llvm::Value* {
			if (call.arguments.size() != 2) {
				error(name + ": expected 2 arguments, got " + std::to_string(call.arguments.size()));
				return nullptr;
			}
			llvm::Value* a = emit(call.arguments[0].get());   // evaluate an argument
			llvm::Value* b = emit(call.arguments[1].get());
			if (!a || !b) return nullptr;
			if (!unify(a, b)) return nullptr;                  // bring both to one numeric type

			llvm::Type* t = a->getType();
			llvm::Value* cond = nullptr;
			if (t->isFloatingPointTy())
				cond = wantMax ? builder.CreateFCmpOGT(a, b) : builder.CreateFCmpOLT(a, b);
			else if (t->isIntegerTy())   // signed compare; unsigned types would need ICmpUGT/ULT
				cond = wantMax ? builder.CreateICmpSGT(a, b) : builder.CreateICmpSLT(a, b);
			else {
				error(name + ": arguments must be numbers");
				return nullptr;
			}
			return builder.CreateSelect(cond, a, b, wantMax ? "max" : "min");
		});
	};
	defineMinMax("Min", false);
	defineMinMax("Max", true);
}
