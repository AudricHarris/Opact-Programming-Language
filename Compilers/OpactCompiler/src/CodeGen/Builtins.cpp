//==========================================================================//
//  Builtins.cpp  -  BUILT-IN FUNCTIONS                                     //
//                                                                          //
//  Everything the language provides "for free" lives here:                 //
//    1. registerBuiltins()  <<< ADD YOUR BUILTINS HERE >>>                 //
//    2. Registration API    (registerBuiltin / registerExternal)           //
//    3. Printf / Println    (format string handling)                       //
//==========================================================================//
#include "CodeGenerator.hpp"

//==========================================================================//
//  1. The builtin list  -  ADD YOUR BUILTINS HERE                          //
//==========================================================================//
void CodeGenerator::registerBuiltins() {
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* ptr = builder.getPtrTy();
	llvm::Type* voidTy = builder.getVoidTy();

	// A) Custom builtins: full control over the emitted IR.
	registerBuiltin("Printf",  [this](CallExpr& c) { return emitFormatted(c, /*newline=*/false); });
	registerBuiltin("Println", [this](CallExpr& c) { return emitFormatted(c, /*newline=*/true);  });

	// B) Thin wrappers over C library functions: ONE line each.
	//                  user name, C name, return, params,   variadic
	registerExternal("Exit", "exit", voidTy, { i32 });
	registerExternal("Puts", "puts", i32,    { ptr });
	registerExternal("Abs",  "abs",  i32,    { i32 });
}

//==========================================================================//
//  2. Registration API                                                     //
//==========================================================================//
void CodeGenerator::registerBuiltin(const std::string& name, BuiltinHandler handler) {
	builtins[name] = std::move(handler);
}

// Declares the C function lazily (only when first used) and coerces the
// arguments to the declared parameter types.
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

//==========================================================================//
//  3. Printf / Println                                                     //
//==========================================================================//

// Parses `src` as a single expression using the normal Lexer + Parser.
ExprPtr CodeGenerator::parseInlineExpression(const std::string& src) {
	try {
		Lexer lexer(src);
		std::vector<Token> toks = lexer.Tokenize();
		Parser parser(std::move(toks), "<format>", nullptr);
		return parser.parseExpression();
	} catch (...) {
		return nullptr;
	}
}

// Picks the printf specifier from the LLVM type and adjusts the value for varargs.
bool CodeGenerator::appendFormatArg(llvm::Value*& v, std::string& fmt) {
	llvm::Type* t = v->getType();
	if (t->isIntegerTy(1)) {            // bool -> "true"/"false"
		fmt += "%s";
		v = builder.CreateSelect(v, builder.CreateGlobalString("true"),
		                         builder.CreateGlobalString("false"), "boolstr");
		return true;
	}
	if (t->isIntegerTy(8))  { fmt += "%c";   v = coerce(v, builder.getInt32Ty()); return true; }
	if (t->isIntegerTy(32)) { fmt += "%d";   return true; }
	if (t->isIntegerTy())   { fmt += "%lld"; v = coerce(v, builder.getInt64Ty()); return true; }
	if (t->isFloatingPointTy()) { fmt += "%f"; v = promoteVarArg(v); return true; }
	if (t->isPointerTy())   { fmt += "%s";   return true; }
	error("Printf: cannot format a value of this type");
	return false;
}

// Printf("x = {x}, sum = {a + b}, positional = {}\n", extraArg)
llvm::Value* CodeGenerator::emitFormatted(CallExpr& call, bool newline) {
	using namespace codegen_detail;

	if (call.arguments.empty()) { error("Printf needs a format string"); return nullptr; }
	auto* lit = dynamic_cast<LiteralExpr*>(call.arguments[0].get());
	if (!lit || lit->kind != LiteralKind::Str) {
		error("Printf: first argument must be a string literal");
		return nullptr;
	}

	std::string tmpl = unescape(stripQuotes(tokenText(lit->lit)));
	if (newline) tmpl += '\n';

	std::string fmt;
	std::vector<llvm::Value*> args;
	size_t nextPositional = 1;

	for (size_t i = 0; i < tmpl.size(); ++i) {
		char c = tmpl[i];

		if (c == '{') {
			if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { fmt += '{'; ++i; continue; }

			// find the matching '}' (braces may nest)
			size_t depth = 1, j = i + 1;
			for (; j < tmpl.size(); ++j) {
				if (tmpl[j] == '{') ++depth;
				else if (tmpl[j] == '}' && --depth == 0) break;
			}
			if (j >= tmpl.size()) { error("Printf: unterminated '{' in format string"); return nullptr; }

			std::string inner = trim(tmpl.substr(i + 1, j - i - 1));
			llvm::Value* v = nullptr;

			if (inner.empty()) {                       // "{}" -> next extra argument
				if (nextPositional >= call.arguments.size()) {
					error("Printf: '{}' has no matching argument");
					return nullptr;
				}
				v = emit(call.arguments[nextPositional++].get());
			} else {                                   // "{any expression}"
				ExprPtr e = parseInlineExpression(inner);
				if (!e) { error("Printf: invalid expression '" + inner + "'"); return nullptr; }
				v = emit(e.get());
			}

			if (!v || !appendFormatArg(v, fmt)) return nullptr;
			args.push_back(v);
			i = j;
		} else if (c == '}') {
			if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') ++i;
			fmt += '}';
		} else if (c == '%') {
			fmt += "%%";
		} else {
			fmt += c;
		}
	}

	if (nextPositional < call.arguments.size())
		std::cerr << "Codegen Warning: Printf has unused extra arguments\n";

	// Declare printf with a pointer type matching your LLVM context
	llvm::Type* charPtrTy = builder.getPtrTy();
	auto printfFn = module->getOrInsertFunction("printf",
		llvm::FunctionType::get(builder.getInt32Ty(), { charPtrTy }, true));

	// Global string, already returned as a plain pointer to the first char.
	// (Opaque pointers have no element type, so no GEP / getContainedType needed.)
	llvm::Value* formatPtr = builder.CreateGlobalStringPtr(fmt, "fmt");

	args.insert(args.begin(), formatPtr);
	return builder.CreateCall(printfFn, args, "printf_ret");
}
