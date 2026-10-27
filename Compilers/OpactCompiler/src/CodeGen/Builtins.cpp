#include "CodeGenerator.hpp"

void CodeGenerator::registerBuiltins() {
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* ptr = builder.getPtrTy();
	llvm::Type* voidTy = builder.getVoidTy();

	registerBuiltin("Printf",  [this](CallExpr& c) { return emitFormatted(c, /*newline=*/false); });
	registerBuiltin("Println", [this](CallExpr& c) { return emitFormatted(c, /*newline=*/true);  });

	registerExternal("Exit", "exit", voidTy, { i32 });
	registerExternal("Puts", "puts", i32,    { ptr });
	registerExternal("Abs",  "abs",  i32,    { i32 });
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

//==========================================================================//
//  3. Printf / Println                                                     //
//==========================================================================//

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

bool CodeGenerator::appendFormatArg(llvm::Value*& v, std::string& fmt) {
	if (!v) return false;
	llvm::Type* t = v->getType();
	
	if (t->isIntegerTy(1)) {
		fmt += "%s";
		v = builder.CreateSelect(v, builder.CreateGlobalString("true"),
		                         builder.CreateGlobalString("false"), "boolstr");
		return true;
	}
	if (t->isIntegerTy(8))  { fmt += "%c";   v = coerce(v, builder.getInt32Ty()); return true; }
	if (t->isIntegerTy(32)) { fmt += "%d";   return true; }
	if (t->isIntegerTy())   { fmt += "%lld"; v = coerce(v, builder.getInt64Ty()); return true; }
	if (t->isFloatingPointTy()) { fmt += "%f"; v = promoteVarArg(v); return true; }
	
	if (t->isPointerTy()) {
		fmt += "%s";
		return true;
	}

	if (t->isStructTy())
		error("Printf: cannot format a whole structure; print its fields instead (e.g. {h.name})");
	else
		error("Printf: cannot format a value of this type");
	return false;
}

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

			if (!v) return nullptr;

			// `{h}` / `{}` naming a structure (or a reference to one) cannot be printed
			std::string varName = inner;
			if (inner.empty()) {
				if (auto* idArg = dynamic_cast<IdentifierExpr*>(call.arguments[nextPositional - 1].get()))
					varName = idArg->name;
			}
			if (!varName.empty()) {
				VarInfo* sv = lookupVar(varName);
				if (sv && sv->structType) {
					error("Printf: '" + varName + "' is a structure; print its fields instead (e.g. {" +
					      varName + ".field})");
					return nullptr;
				}
			}
			if (!appendFormatArg(v, fmt)) return nullptr;
			args.push_back(v);
			i = j;
		} else if (c == '}') {
			if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') ++i;
			fmt += '}';
		} else if (c == '%') {
			fmt += "%%";
		} else if (c == '\\') {
			if (i + 1 < tmpl.size() && (tmpl[i + 1] == '{' || tmpl[i + 1] == '}')) {
				fmt += tmpl[i + 1];
				++i;
			} else if (i + 1 < tmpl.size()) {
				fmt += tmpl[i + 1];
				++i;
			}
		}
		else {
			fmt += c;
		}
	}

	if (nextPositional < call.arguments.size())
		std::cerr << "Codegen Warning: Printf has unused extra arguments\n";

	llvm::Type* charPtrTy = builder.getPtrTy();
	auto printfFn = module->getOrInsertFunction("printf",
			llvm::FunctionType::get(builder.getInt32Ty(), { charPtrTy }, true));

	llvm::Value* formatPtr = builder.CreateGlobalString(fmt, "fmt");

	args.insert(args.begin(), formatPtr);
	return builder.CreateCall(printfFn, args, "printf_ret");
}
