//==========================================================================//
//  CodeGenerator.cpp  -  CORE IMPLEMENTATION                               //
//                                                                          //
//  Sections:                                                               //
//    1. Constructor                                                        //
//    2. Type system (mapType, toBool, coerce, unify, binary operators)     //
//    3. Helpers (errors, scopes, blocks, functions, field access)          //
//    4. Visitors (one per AST node)                                        //
//                                                                          //
//  Builtins (Printf, Println, C wrappers) live in Builtins.cpp.            //
//==========================================================================//
#include "CodeGenerator.hpp"
#include <llvm/IR/DerivedTypes.h>

//==========================================================================//
//  1. Constructor                                                          //
//==========================================================================//
CodeGenerator::CodeGenerator(const std::string& moduleName, llvm::LLVMContext& ctx)
	: context(ctx), builder(ctx), module(std::make_unique<llvm::Module>(moduleName, ctx)) {
	scopes.emplace_back(); // global scope
	registerBuiltins();
}

//==========================================================================//
//  2. Type system (single place to extend when adding types)               //
//==========================================================================//
llvm::Type* CodeGenerator::mapType(const TypeDesc& d) {
	if (d.isReference) {
		return builder.getPtrTy();
	}
	switch (d.type) {
		case DataType::Int:   return builder.getInt32Ty();
		case DataType::Float: return builder.getDoubleTy();
		case DataType::Bool:  return builder.getInt1Ty();
		case DataType::Char:  return builder.getInt8Ty();
		case DataType::Str:   return builder.getPtrTy();
		case DataType::Void:  return builder.getVoidTy();
		case DataType::Custom: {
			auto it = structs.find(d.customTypeName);
			if (it != structs.end()) return it->second.type;
			error("Unknown type '" + d.customTypeName + "'");
			return nullptr;
		}
	}
	return nullptr;
}

// For a reference type, the type the pointer points at (nullptr otherwise).
llvm::Type* CodeGenerator::pointeeOf(const TypeDesc& d) {
	if (!d.isReference) return nullptr;
	TypeDesc inner = d;
	inner.isReference = false;
	llvm::Type* p = mapType(inner);
	return (p && !p->isVoidTy()) ? p : nullptr;
}

// Anything -> i1 (non-zero / non-null is true).
llvm::Value* CodeGenerator::toBool(llvm::Value* v) {
	if (!v) return nullptr;
	llvm::Type* t = v->getType();
	if (t->isIntegerTy(1)) return v;
	if (t->isFloatingPointTy())
		return builder.CreateFCmpUNE(v, llvm::ConstantFP::get(t, 0.0), "tobool");
	return builder.CreateIsNotNull(v, "tobool"); // ints and pointers
}

llvm::Value* CodeGenerator::coerce(llvm::Value* v, llvm::Type* to) {
	if (!v || !to) return nullptr;
	llvm::Type* from = v->getType();
	if (from == to) return v;

	if (from->isPointerTy() && to->isPointerTy())
		return builder.CreatePointerCast(v, to, "ptrcast");

	if (to->isIntegerTy(1)) return toBool(v);

	if (from->isIntegerTy() && to->isIntegerTy()) {
		unsigned fb = from->getIntegerBitWidth(), tb = to->getIntegerBitWidth();
		if (tb < fb) return builder.CreateTrunc(v, to, "trunc");
		return fb == 1 ? builder.CreateZExt(v, to, "zext") : builder.CreateSExt(v, to, "sext");
	}
	if (from->isIntegerTy() && to->isFloatingPointTy())
		return from->isIntegerTy(1) ? builder.CreateUIToFP(v, to, "uitofp")
		                            : builder.CreateSIToFP(v, to, "sitofp");
	if (from->isFloatingPointTy() && to->isIntegerTy())
		return builder.CreateFPToSI(v, to, "fptosi");
	if (from->isFloatingPointTy() && to->isFloatingPointTy())
		return from->getPrimitiveSizeInBits() < to->getPrimitiveSizeInBits()
			? builder.CreateFPExt(v, to, "fpext") : builder.CreateFPTrunc(v, to, "fptrunc");

	error("Cannot convert between these types");
	return nullptr;
}

// C variadic promotion: small ints -> i32 (floats are already double).
llvm::Value* CodeGenerator::promoteVarArg(llvm::Value* v) {
	llvm::Type* t = v->getType();
	if (t->isIntegerTy() && t->getIntegerBitWidth() < 32) return coerce(v, builder.getInt32Ty());
	if (t->isFloatTy()) return builder.CreateFPExt(v, builder.getDoubleTy(), "fpext");
	return v;
}

// Brings both operands to a common numeric type.
bool CodeGenerator::unify(llvm::Value*& l, llvm::Value*& r) {
	llvm::Type* lt = l->getType();
	llvm::Type* rt = r->getType();
	if (lt == rt) return true;

	if (lt->isFloatingPointTy() || rt->isFloatingPointTy()) {
		llvm::Type* d = builder.getDoubleTy();
		l = coerce(l, d);
		r = coerce(r, d);
		return l && r;
	}
	if (lt->isIntegerTy() && rt->isIntegerTy()) {
		llvm::Type* wide = lt->getIntegerBitWidth() >= rt->getIntegerBitWidth() ? lt : rt;
		l = coerce(l, wide);
		r = coerce(r, wide);
		return l && r;
	}

	if (lt->isPointerTy() && !rt->isPointerTy()) {
		l = builder.CreateLoad(rt, l, "unref_l");
		lt = l->getType();
	} else if (rt->isPointerTy() && !lt->isPointerTy()) {
		r = builder.CreateLoad(lt, r, "unref_r");
		rt = r->getType();
	}
	if (lt == rt) return true;

	error("Operand types do not match");
	return false;
}

llvm::Value* CodeGenerator::emitBinary(BinaryOp op, llvm::Value* l, llvm::Value* r) {
	if (op == BinaryOp::And) return builder.CreateAnd(toBool(l), toBool(r), "andtmp");
	if (op == BinaryOp::Or)  return builder.CreateOr(toBool(l),  toBool(r), "ortmp");

	llvm::Type* lt = l->getType();
	llvm::Type* rt = r->getType();

	// String/pointer concatenation BEFORE unify so different pointer types don't trigger errors
	if (op == BinaryOp::Add && !lt->isFloatingPointTy() && !lt->isIntegerTy() &&
			!rt->isFloatingPointTy() && !rt->isIntegerTy()) {
		auto charPtrTy = builder.getPtrTy();
		auto sizeTy = builder.getInt64Ty();
		auto strlenFn = module->getOrInsertFunction("strlen", llvm::FunctionType::get(sizeTy, { charPtrTy }, false));
		auto mallocFn = module->getOrInsertFunction("malloc", llvm::FunctionType::get(charPtrTy, { sizeTy }, false));
		auto memcpyFn = module->getOrInsertFunction("memcpy", llvm::FunctionType::get(charPtrTy, { charPtrTy, charPtrTy, sizeTy }, false));
		llvm::Value* str1 = builder.CreatePointerCast(l, charPtrTy, "str1");
		llvm::Value* str2 = builder.CreatePointerCast(r, charPtrTy, "str2");
		llvm::Value* len1 = builder.CreateCall(strlenFn, { str1 }, "len1");
		llvm::Value* len2 = builder.CreateCall(strlenFn, { str2 }, "len2");
		llvm::Value* totalLen = builder.CreateAdd(
				builder.CreateAdd(len1, len2, "addlen"),
				llvm::ConstantInt::get(sizeTy, 1),
				"totallen");
		llvm::Value* dest = builder.CreateCall(mallocFn, { totalLen }, "dest");
		builder.CreateCall(memcpyFn, { dest, str1, len1 });
		llvm::Value* destOffset = builder.CreateInBoundsGEP(builder.getInt8Ty(), dest, { len1 }, "dest_offset");
		llvm::Value* len2Plus1 = builder.CreateAdd(len2, llvm::ConstantInt::get(sizeTy, 1), "len2_plus_1");
		builder.CreateCall(memcpyFn, { destOffset, str2, len2Plus1 });
		return dest;
	}

	if (!unify(l, r)) return nullptr;
	lt = l->getType();
	rt = r->getType();
	bool fp = lt->isFloatingPointTy();

	switch (op) {
		case BinaryOp::Add: return fp ? builder.CreateFAdd(l, r, "addtmp") : builder.CreateAdd(l, r, "addtmp");
		case BinaryOp::Sub: return fp ? builder.CreateFSub(l, r, "subtmp") : builder.CreateSub(l, r, "subtmp");
		case BinaryOp::Mul: return fp ? builder.CreateFMul(l, r, "multmp") : builder.CreateMul(l, r, "multmp");
		case BinaryOp::Div: return fp ? builder.CreateFDiv(l, r, "divtmp") : builder.CreateSDiv(l, r, "divtmp");
		case BinaryOp::Mod: return fp ? builder.CreateFRem(l, r, "modtmp") : builder.CreateSRem(l, r, "modtmp");

		case BinaryOp::Power: {
			llvm::Type* d = builder.getDoubleTy();
			llvm::Value* res = builder.CreateIntrinsic(llvm::Intrinsic::pow, { d },
					{ coerce(l, d), coerce(r, d) }, nullptr, "powtmp");
			return fp ? res : coerce(res, lt);
		}

		case BinaryOp::BitAnd:
			if (fp) { error("Bitwise operator needs integer operands"); return nullptr; }
			return builder.CreateAnd(l, r, "bandtmp");

		case BinaryOp::Eq: return fp ? builder.CreateFCmpOEQ(l, r, "cmptmp") : builder.CreateICmpEQ(l, r, "cmptmp");
		case BinaryOp::Ne: return fp ? builder.CreateFCmpUNE(l, r, "cmptmp") : builder.CreateICmpNE(l, r, "cmptmp");
		case BinaryOp::Lt: return fp ? builder.CreateFCmpOLT(l, r, "cmptmp") : builder.CreateICmpSLT(l, r, "cmptmp");
		case BinaryOp::Gt: return fp ? builder.CreateFCmpOGT(l, r, "cmptmp") : builder.CreateICmpSGT(l, r, "cmptmp");
		case BinaryOp::Le: return fp ? builder.CreateFCmpOLE(l, r, "cmptmp") : builder.CreateICmpSLE(l, r, "cmptmp");
		case BinaryOp::Ge: return fp ? builder.CreateFCmpOGE(l, r, "cmptmp") : builder.CreateICmpSGE(l, r, "cmptmp");
		default: break;
	}
	error("Unsupported binary operator");
	return nullptr;
}

//==========================================================================//
//  3. Helpers                                                              //
//==========================================================================//
void CodeGenerator::error(const std::string& msg) {
	++errorCount;
	std::cerr << "Codegen Error: " << msg << "\n";
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

// Evaluate an expression and return its value (nullptr if none / error).
llvm::Value* CodeGenerator::emit(Expression* e) {
	if (!e) return nullptr;
	lastValue = nullptr;
	e->accept(*this);
	return lastValue;
}

void CodeGenerator::pushScope() { scopes.emplace_back(); }
void CodeGenerator::popScope()  { scopes.pop_back(); }

CodeGenerator::VarInfo* CodeGenerator::lookupVar(const std::string& name) {
	for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
		auto f = it->find(name);
		if (f != it->end()) return &f->second;
	}
	return nullptr;
}

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

// Computes the address of `obj.member` and the type stored there.
// If the field is a reference, the stored pointer is followed, so `addr`
// is the address of the referenced object and `type` is its type.
bool CodeGenerator::emitFieldAddr(MemberAccessExpr& ma, llvm::Value*& addr, llvm::Type*& type) {
	llvm::Value* objPtr = nullptr;
	llvm::StructType* structTy = nullptr;

	if (auto* objId = dynamic_cast<IdentifierExpr*>(ma.object.get())) {
		VarInfo* var = lookupVar(objId->name);
		if (!var) {
			error("Undefined variable '" + objId->name + "'");
			return false;
		}
		// References/pointers to structs (like parameters `&mut Human self`)
		if (var->type->isPointerTy() && var->structType != nullptr) {
			objPtr   = builder.CreateLoad(builder.getPtrTy(), var->addr, objId->name + "_load");
			structTy = var->structType;
		} else {
			objPtr   = var->addr;
			structTy = var->structType ? var->structType : llvm::dyn_cast<llvm::StructType>(var->type);
		}
	} else {
		// Nested expressions: we get an address but cannot know the struct type yet.
		objPtr = emit(ma.object.get());
	}

	if (!structTy || !objPtr) {
		error("Member access '.' is only supported on structures");
		return false;
	}

	std::string structName = structTy->getName().str();
	auto it = structs.find(structName);
	if (it == structs.end()) {
		error("Unknown structure type metadata for '" + structName + "'");
		return false;
	}

	const StructInfo& info = it->second;
	auto fieldIt = std::find(info.fieldNames.begin(), info.fieldNames.end(), ma.memberName);
	if (fieldIt == info.fieldNames.end()) {
		error("Structure '" + structName + "' has no field named '" + ma.memberName + "'");
		return false;
	}
	size_t idx = static_cast<size_t>(std::distance(info.fieldNames.begin(), fieldIt));

	if (idx >= info.fieldIsRef.size() || idx >= info.fieldPointee.size()) {
		error("Internal error: incomplete metadata for structure '" + structName + "'");
		return false;
	}

	llvm::Value* fieldPtr = builder.CreateStructGEP(structTy, objPtr, idx, ma.memberName + "_ptr");
	if (info.fieldIsRef[idx] && info.fieldPointee[idx]) {
		addr = builder.CreateLoad(builder.getPtrTy(), fieldPtr, ma.memberName + "_ref");
		type = info.fieldPointee[idx];
	} else {
		addr = fieldPtr;
		type = structTy->getElementType(idx);
	}
	return true;
}

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

//==========================================================================//
//  4. Visitors                                                             //
//==========================================================================//

// ----- Literals & variables -----------------------------------------------
void CodeGenerator::visit(LiteralExpr& node) {
	using namespace codegen_detail;
	std::string text = tokenText(node.lit);

	switch (node.kind) {
		case LiteralKind::Int: {
			text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
			int base = 10;
			size_t skip = 0;
			switch (node.base) {
				case NumericBase::Hexadecimal: base = 16; skip = 2; break;
				case NumericBase::Octal:       base = 8;  skip = 2; break;
				case NumericBase::Binary:      base = 2;  skip = 2; break;
				default: break;
			}
			try {
				unsigned long long v = std::stoull(text.substr(skip), nullptr, base);
				llvm::Type* ty = (v > 0x7FFFFFFFULL) ? builder.getInt64Ty() : builder.getInt32Ty();
				lastValue = llvm::ConstantInt::get(ty, v, false);
			} catch (...) {
				error("Invalid integer literal '" + text + "'");
				lastValue = nullptr;
			}
			break;
		}
		case LiteralKind::Float:
			try {
				lastValue = llvm::ConstantFP::get(builder.getDoubleTy(), std::stod(text));
			} catch (...) {
				error("Invalid float literal '" + text + "'");
				lastValue = nullptr;
			}
			break;
		case LiteralKind::Bool:
			lastValue = builder.getInt1(text == "true");
			break;
		case LiteralKind::Char: {
			std::string c = unescape(stripQuotes(text));
			lastValue = builder.getInt8(c.empty() ? 0 : static_cast<uint8_t>(c[0]));
			break;
		}
		case LiteralKind::Str:
			lastValue = builder.CreateGlobalString(unescape(stripQuotes(text)), "str");
			break;
	}
}

void CodeGenerator::visit(IdentifierExpr& node) {
	VarInfo* var = lookupVar(node.name);
	if (!var) {
		error("Undefined variable '" + node.name + "'");
		lastValue = nullptr;
		return;
	}
	if (var->pointee) {
		// Reference to a primitive: load the pointer, then the value behind it.
		llvm::Value* p = builder.CreateLoad(builder.getPtrTy(), var->addr, node.name + "_ref");
		lastValue = builder.CreateLoad(var->pointee, p, node.name);
		return;
	}
	lastValue = builder.CreateLoad(var->type, var->addr, node.name);
}

void CodeGenerator::visit(VarDeclExpr& node) {
	std::string name = tokenText(node.name);
	if (!currentFn) { error("Variable '" + name + "' declared outside of a function"); return; }

	// A declared reference wants an address, everything else wants a value.
	llvm::Value* init = nullptr;
	if (node.expr) init = node.type.isReference ? emitRef(node.expr.get()) : emit(node.expr.get());
	if (node.expr && !init) { lastValue = nullptr; return; }

	llvm::Type* ty = (node.type.type == DataType::Void && init) ? init->getType() : mapType(node.type);
	if (!ty || ty->isVoidTy()) { error("Invalid type for variable '" + name + "'"); return; }

	llvm::Value* initVal = nullptr;
	if (init) {
		if (ty->isStructTy() && init->getType()->isPointerTy()) {
			initVal = builder.CreateLoad(ty, init, "structinit");
		} else {
			initVal = coerce(init, ty);
		}
	} else {
		initVal = llvm::Constant::getNullValue(ty);
	}
	if (!initVal) return;

	llvm::AllocaInst* slot = createEntryAlloca(currentFn, ty, name);
	builder.CreateStore(initVal, slot);

	llvm::StructType* st = nullptr;
	if (node.type.type == DataType::Custom) {
		auto it = structs.find(node.type.customTypeName);
		if (it != structs.end()) st = it->second.type;
	}

	// For references to non-struct types remember what the pointer points at.
	llvm::Type* pointee = pointeeOf(node.type);
	if (pointee && pointee->isStructTy()) pointee = nullptr;

	scopes.back()[name] = { slot, ty, st, pointee };
	lastValue = initVal;
}

void CodeGenerator::visit(AssignExpr& node) {
	lastValue = nullptr;
	llvm::Value* targetPtr = nullptr;
	llvm::Type* targetType = nullptr;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.target.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		if (var->pointee) {
			// `r = value` on a reference writes to the referenced object.
			targetPtr  = builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_ref");
			targetType = var->pointee;
		} else {
			targetPtr  = var->addr;
			targetType = var->type;
		}
	}
	else if (auto* ma = dynamic_cast<MemberAccessExpr*>(node.target.get())) {
		if (!emitFieldAddr(*ma, targetPtr, targetType)) return;
	}
	else {
		error("Only variables or structure fields can be assigned to");
		return;
	}

	llvm::Value* v = coerce(emit(node.value.get()), targetType);
	if (!v) {
		lastValue = nullptr;
		return;
	}

	builder.CreateStore(v, targetPtr);
	lastValue = v;
}

void CodeGenerator::visit(BorrowExpr& node) {
	lastValue = nullptr;
	if (auto* id = dynamic_cast<IdentifierExpr*>(node.expr.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		// Re-borrowing a reference yields the address it already holds,
		// not the address of the slot that stores the pointer.
		if (var->pointee || (var->structType && var->type->isPointerTy()))
			lastValue = builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_reborrow");
		else
			lastValue = var->addr; // address of the variable being borrowed
	} else {
		error("Cannot borrow non-lvalue expression");
	}
}

// ----- Operators ----------------------------------------------------------
void CodeGenerator::visit(PostfixExpr& node) {
	lastValue = nullptr;
	llvm::Value* targetAddr = nullptr;
	llvm::Type* targetType = nullptr;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.expr.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		if (var->pointee) {
			targetAddr = builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_ref");
			targetType = var->pointee;
		} else {
			targetAddr = var->addr;
			targetType = var->type;
		}
	}
	else if (auto* ma = dynamic_cast<MemberAccessExpr*>(node.expr.get())) {
		if (!emitFieldAddr(*ma, targetAddr, targetType)) return;
	}
	else {
		error("++/-- requires a variable identifier or struct field");
		return;
	}

	llvm::Value* old = builder.CreateLoad(targetType, targetAddr, "old");
	bool inc = node.op == PostfixOp::Increment;
	llvm::Value* updated;

	if (targetType->isFloatingPointTy()) {
		llvm::Value* one = llvm::ConstantFP::get(targetType, 1.0);
		updated = inc ? builder.CreateFAdd(old, one, "inc") : builder.CreateFSub(old, one, "dec");
	} else if (targetType->isIntegerTy()) {
		llvm::Value* one = llvm::ConstantInt::get(targetType, 1);
		updated = inc ? builder.CreateAdd(old, one, "inc") : builder.CreateSub(old, one, "dec");
	} else {
		error("++/-- requires a numeric variable or reference");
		return;
	}

	builder.CreateStore(updated, targetAddr);
	lastValue = old; // postfix yields the previous value
}

void CodeGenerator::visit(BinaryExpr& node) {
	llvm::Value* l = emit(node.left.get());
	llvm::Value* r = emit(node.right.get());
	lastValue = (l && r) ? emitBinary(node.op, l, r) : nullptr;
}

void CodeGenerator::visit(UnaryExpr& node) {
	llvm::Value* v = emit(node.expr.get());
	if (!v) { lastValue = nullptr; return; }

	if (node.op == UnaryOp::Negate) {
		lastValue = v->getType()->isFloatingPointTy() ? builder.CreateFNeg(v, "negtmp")
			: builder.CreateNeg(v, "negtmp");
	} else {
		lastValue = builder.CreateNot(toBool(v), "nottmp");
	}
}

void CodeGenerator::visit(CastExpr& node) {
	llvm::Value* v = emit(node.expr.get());
	llvm::Type* to = mapType(node.castType);
	lastValue = (v && to && !to->isVoidTy()) ? coerce(v, to) : nullptr;
}

// ----- Blocks & control flow ----------------------------------------------
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

	llvm::Value* v = (node.value && *node.value) ? emit(node.value->get()) : nullptr;
	llvm::Type* rt = currentFn->getReturnType();

	if (rt->isVoidTy()) {
		builder.CreateRetVoid();
	} else {
		v = v ? coerce(v, rt) : llvm::Constant::getNullValue(rt);
		if (!v) return;
		builder.CreateRet(v);
	}
	startDeadBlock();
	lastValue = nullptr;
}

// ----- Declarations -------------------------------------------------------
void CodeGenerator::visit(FunctionDeclExpr& node) {
	llvm::Function* fn = declareFunction(node);
	if (!fn || !node.body) return;
	if (!fn->empty()) { error("Redefinition of function '" + node.name + "'"); return; }

	// Save state so nested declarations don't corrupt the outer function.
	auto savedIP   = builder.saveIP();
	auto* savedFn  = currentFn;
	auto savedLoop = std::move(loopStack);
	loopStack.clear();

	currentFn = fn;
	builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));

	pushScope();
	size_t idx = 0;
	for (auto& arg : fn->args()) {
		const auto& paramDesc = node.params[idx++];
		const std::string& pname = paramDesc.name;
		arg.setName(pname);

		llvm::AllocaInst* slot = createEntryAlloca(fn, arg.getType(), pname);
		builder.CreateStore(&arg, slot);

		llvm::StructType* st = nullptr;
		auto it = structs.find(paramDesc.type.customTypeName);
		if (it != structs.end()) st = it->second.type;

		llvm::Type* pointee = pointeeOf(paramDesc.type);
		if (pointee && pointee->isStructTy()) pointee = nullptr;

		scopes.back()[pname] = { slot, arg.getType(), st, pointee };
	}

	emit(node.body.get());
	emitDefaultReturn();
	popScope();

	currentFn = savedFn;
	loopStack = std::move(savedLoop);
	builder.restoreIP(savedIP);
	lastValue = nullptr;
}

void CodeGenerator::visit(StructDeclExpr& node) {
	auto it = structs.find(node.name);
	if (it == structs.end()) {
		StructInfo info;
		info.type = llvm::StructType::create(context, node.name);
		it = structs.emplace(node.name, std::move(info)).first;
	}

	// Already defined (body set): do not append the metadata twice.
	if (!it->second.type->isOpaque()) return;

	StructInfo& info = it->second;
	info.fieldNames.clear();
	info.fieldIsRef.clear();
	info.fieldPointee.clear();

	std::vector<llvm::Type*> fieldTypes;
	for (const auto& f : node.attributes) {
		llvm::Type* ft = mapType(f.type); // reference -> ptr
		if (!ft || ft->isVoidTy()) { error("Invalid type for field '" + f.name + "'"); return; }
		fieldTypes.push_back(ft);
		info.fieldNames.push_back(f.name);
		info.fieldIsRef.push_back(f.type.isReference);
		info.fieldPointee.push_back(pointeeOf(f.type)); // nullptr for non-reference fields
	}
	info.type->setBody(fieldTypes);
}

void CodeGenerator::visit(StructCreateExpr& node) {
	lastValue = nullptr;

	auto* id = dynamic_cast<IdentifierExpr*>(node.structObj.get());
	if (!id) {
		error("Only direct structure creation by name is supported");
		return;
	}
	if (!currentFn) {
		error("Structure creation outside of a function");
		return;
	}

	auto it = structs.find(id->name);
	if (it == structs.end()) {
		error("Unknown structure type '" + id->name + "'");
		return;
	}
	const StructInfo& info = it->second;
	llvm::StructType* llvmStructTy = info.type;

	if (llvmStructTy->isOpaque()) {
		error("Structure '" + id->name + "' is used before its definition");
		return;
	}

	if (llvmStructTy->getNumElements() != node.arguments.size()) {
		error("Structure '" + id->name + "' expects " +
				std::to_string(llvmStructTy->getNumElements()) +
				" argument(s), but got " + std::to_string(node.arguments.size()));
		return;
	}

	// Entry-block alloca: no stack growth when this runs inside a loop.
	llvm::AllocaInst* structAlloca = createEntryAlloca(currentFn, llvmStructTy, id->name + "_tmp");
	builder.CreateStore(llvm::Constant::getNullValue(llvmStructTy), structAlloca);

	for (size_t i = 0; i < node.arguments.size(); ++i) {
		// Reference fields want an address, normal fields want a value.
		bool isRef = i < info.fieldIsRef.size() && info.fieldIsRef[i];
		llvm::Value* argVal = isRef ? emitRef(node.arguments[i].get())
		                            : emit(node.arguments[i].get());
		if (!argVal) return;

		argVal = coerce(argVal, llvmStructTy->getElementType(i));
		if (!argVal) return;

		llvm::Value* fieldPtr = builder.CreateStructGEP(llvmStructTy, structAlloca, i,
				id->name + "_field_" + std::to_string(i));
		builder.CreateStore(argVal, fieldPtr);
	}

	lastValue = structAlloca;
}

void CodeGenerator::visit(MemberAccessExpr& node) {
	lastValue = nullptr;

	llvm::Value* addr = nullptr;
	llvm::Type* ty = nullptr;
	if (!emitFieldAddr(node, addr, ty)) return;

	lastValue = builder.CreateLoad(ty, addr, node.memberName);
}

// ----- Calls --------------------------------------------------------------
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
		std::vector<llvm::Value*> args;
		size_t i = 0;
		for (auto& a : node.arguments) {
			llvm::Type* pt = callee->getFunctionType()->getParamType(i++);
			// pointer parameter -> pass the address, never the dereferenced value
			llvm::Value* v = pt->isPointerTy() ? emitRef(a.get()) : emit(a.get());
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

void CodeGenerator::visit(ImportExpr&) { /* handled by the compiler pipeline */ }
