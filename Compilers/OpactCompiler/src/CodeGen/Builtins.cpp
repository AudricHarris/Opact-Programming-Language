#include "CodeGenerator.hpp"
#include <cstdint>

void CodeGenerator::registerBuiltins() {
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* ptr = builder.getPtrTy();
	llvm::Type* voidTy = builder.getVoidTy();

	registerBuiltin("Printf",  [this](CallExpr& c) { return emitFormatted(c, /*newline=*/false); });
	registerBuiltin("Println", [this](CallExpr& c) { return emitFormatted(c, /*newline=*/true);  });
	registerBuiltin("Range",   [this](CallExpr& c) { return emitRange(c); });
	registerBuiltin("Random",  [this](CallExpr& c) { return emitRandom(c); });

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
//  Printf / Println                                                        //
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

//==========================================================================//
//  Range / Random                                                          //
//==========================================================================//

// Integer constant (`5`, `-5`) that fits in i32.
bool CodeGenerator::constI32(Expression* e, long long& out) {
	bool neg = false;
	unsigned long long mag = 0;
	if (!evalConstInt(e, neg, mag)) return false;
	if (mag > (neg ? 2147483648ULL : 2147483647ULL)) return false;
	out = neg ? -static_cast<long long>(mag) : static_cast<long long>(mag);
	return true;
}

// Range(end) / Range(start, end) / Range(start, end, step) -> [N x i32]
// `end` is exclusive. Arguments must be integer constants so the array
// length is known at compile time.
llvm::Value* CodeGenerator::emitRange(CallExpr& call) {
	if (!currentFn) { error("Range used outside of a function"); return nullptr; }

	size_t n = call.arguments.size();
	if (n < 1 || n > 3) {
		error("Range expects 1 to 3 arguments, got " + std::to_string(n));
		return nullptr;
	}

	long long v[3] = { 0, 0, 1 };
	for (size_t i = 0; i < n; ++i) {
		if (!constI32(call.arguments[i].get(), v[i])) {
			error("Range arguments must be integer constants (array sizes are fixed at compile time)");
			return nullptr;
		}
	}

	long long start = 0, end = 0, step = 1;
	if (n == 1)      { end = v[0]; }
	else if (n == 2) { start = v[0]; end = v[1]; }
	else             { start = v[0]; end = v[1]; step = v[2]; }

	long long count = 0;
	std::string err;
	if (!rangeLength(start, end, step, count, err)) { error(err); return nullptr; }

	std::vector<int32_t> values;
	values.reserve(static_cast<size_t>(count));
	for (long long i = 0; i < count; ++i)
		values.push_back(static_cast<int32_t>(start + i * step));

	llvm::Constant* data = llvm::ConstantDataArray::get(context, values); // [count x i32]
	llvm::AllocaInst* slot = createEntryAlloca(currentFn, data->getType(), "range");
	builder.CreateStore(data, slot);
	return slot; // address of the array, like an array literal
}

llvm::Value* CodeGenerator::emitRandom(CallExpr& call) {
	if (!currentFn) { error("Random used outside of a function"); return nullptr; }
	if (!call.arguments.empty()) { error("Random takes no arguments"); return nullptr; }

	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* i64 = builder.getInt64Ty();
	llvm::Type* ptr = builder.getPtrTy();

	auto clockGetTimeFn = module->getOrInsertFunction("clock_gettime", 
		llvm::FunctionType::get(i32, { i32, ptr }, false));

	llvm::Type* timespecType = llvm::StructType::get(context, { i64, i64 });
	llvm::AllocaInst* tsAlloca = createEntryAlloca(currentFn, timespecType, "ts");

	llvm::Value* clockId = llvm::ConstantInt::get(i32, 1);
	llvm::Value* tsCast = builder.CreatePointerCast(tsAlloca, ptr, "ts_ptr");
	builder.CreateCall(clockGetTimeFn, { clockId, tsCast });

	llvm::Value* nsecPtr = builder.CreateStructGEP(timespecType, tsAlloca, 1, "nsec_ptr");
	llvm::Value* nsecVal = builder.CreateLoad(i64, nsecPtr, "nsec_val");

	llvm::Value* mult = builder.CreateMul(nsecVal, llvm::ConstantInt::get(i64, 1103515245LL), "lcg_mult");
	llvm::Value* add  = builder.CreateAdd(mult, llvm::ConstantInt::get(i64, 12345LL), "lcg_add");

	llvm::Value* masked = builder.CreateAnd(add, llvm::ConstantInt::get(i64, 0x7FFFFFFF), "rand_mask");

	llvm::Value* asDouble = builder.CreateSIToFP(masked, builder.getDoubleTy(), "rand_f");
	return builder.CreateFDiv(asDouble,
			llvm::ConstantFP::get(builder.getDoubleTy(), 2147483648.0), "rand_unit");
}
