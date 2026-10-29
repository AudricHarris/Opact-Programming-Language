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
#include <algorithm>
#include <cstdint>

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
	llvm::Type* base = nullptr;
	switch (d.type) {
		case DataType::I8:  case DataType::U8:  base = builder.getInt8Ty();   break;
		case DataType::I16: case DataType::U16: base = builder.getInt16Ty();  break;
		case DataType::I32: case DataType::Int:  base = builder.getInt32Ty();   break;
		case DataType::U32: case DataType::UInt: base = builder.getInt32Ty();  break;
		case DataType::I64: case DataType::U64: base = builder.getInt64Ty();  break;
		case DataType::F32: base = builder.getFloatTy();   break;
		case DataType::F64: case DataType::Float: base = builder.getDoubleTy(); break;
		case DataType::Bool:  base = builder.getInt1Ty();    break;
		case DataType::Char:  base = builder.getInt8Ty();    break;
		case DataType::Str:   base = builder.getPtrTy();     break;
		case DataType::Void:  base = builder.getVoidTy();    break;
		case DataType::Custom: {
			auto it = structs.find(d.customTypeName);
			if (it != structs.end()) { base = it->second.type; break; }
			error("Unknown type '" + d.customTypeName + "'");
			return nullptr;
		}
	}
	if (!base) return nullptr;

	for (auto it = d.dim.rbegin(); it != d.dim.rend(); ++it) {
		base = llvm::ArrayType::get(base, *it);
	}
	return base;
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
		if (fb == 1) return builder.CreateZExt(v, to, "zext");

		return builder.CreateSExt(v, to, "sext");
	}
	if (from->isIntegerTy() && to->isFloatingPointTy())
		return from->isIntegerTy(1) ? builder.CreateUIToFP(v, to, "uitofp")
			: builder.CreateSIToFP(v, to, "sitofp");
	if (from->isFloatingPointTy() && to->isIntegerTy())
		return builder.CreateFPToSI(v, to, "fptosi");
	if (from->isFloatingPointTy() && to->isFloatingPointTy())
		return from->getPrimitiveSizeInBits() < to->getPrimitiveSizeInBits()
			? builder.CreateFPExt(v, to, "fpext") : builder.CreateFPTrunc(v, to, "fptrunc");

	if (from->isPointerTy() && to->isStructTy()) {
		return builder.CreateLoad(to, v, "struct_coerce_load");
	}

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

llvm::Value* CodeGenerator::emitBinary(BinaryOp op, llvm::Value* l, llvm::Value* r, bool isUnsigned) {
	if (op == BinaryOp::And) return builder.CreateAnd(toBool(l), toBool(r), "andtmp");
	if (op == BinaryOp::Or)  return builder.CreateOr(toBool(l),  toBool(r), "ortmp");

	llvm::Type* lt = l->getType();
	llvm::Type* rt = r->getType();

	// String/pointer concatenation BEFORE unify so different pointer types don't trigger errors
	if (op == BinaryOp::Add && !lt->isFloatingPointTy() && !lt->isIntegerTy() &&
			!rt->isFloatingPointTy() && !rt->isIntegerTy()) {
		if (!currentFn) {
			error("String concatenation outside of a function");
			return nullptr;
		}
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

		// Own the heap string through a null-initialised entry-block slot.
		llvm::AllocaInst* slot = createHeapSlot("heap_str");
		builder.CreateStore(dest, slot);
		registerCleanup(slot);

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
		case BinaryOp::Div:
			return fp ? builder.CreateFDiv(l, r, "divtmp")
				: (isUnsigned ? builder.CreateUDiv(l, r, "udivtmp") : builder.CreateSDiv(l, r, "sdivtmp"));
		case BinaryOp::Mod:
			return fp ? builder.CreateFRem(l, r, "modtmp")
				: (isUnsigned ? builder.CreateURem(l, r, "uremtmp") : builder.CreateSRem(l, r, "sremtmp"));

		case BinaryOp::Eq: return fp ? builder.CreateFCmpOEQ(l, r, "cmptmp") : builder.CreateICmpEQ(l, r, "cmptmp");
		case BinaryOp::Ne: return fp ? builder.CreateFCmpUNE(l, r, "cmptmp") : builder.CreateICmpNE(l, r, "cmptmp");
		case BinaryOp::Lt: return fp ? builder.CreateFCmpOLT(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpULT(l, r, "cmptmp") : builder.CreateICmpSLT(l, r, "cmptmp"));
		case BinaryOp::Gt: return fp ? builder.CreateFCmpOGT(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpUGT(l, r, "cmptmp") : builder.CreateICmpSGT(l, r, "cmptmp"));
		case BinaryOp::Le: return fp ? builder.CreateFCmpOLE(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpULE(l, r, "cmptmp") : builder.CreateICmpSLE(l, r, "cmptmp"));
		case BinaryOp::Ge: return fp ? builder.CreateFCmpOGE(l, r, "cmptmp") : (isUnsigned ? builder.CreateICmpUGE(l, r, "cmptmp") : builder.CreateICmpSGE(l, r, "cmptmp"));
		default: break;
	}
	error("Unsupported binary operator");
	return nullptr;
}

llvm::Value* CodeGenerator::emitLValue(Expression* e) {
	if (!e) return nullptr;

	// 1. Identifier (e.g., `x`)
	if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return nullptr;
		}
		if (var->pointee || (var->structType && var->type->isPointerTy()))
			return builder.CreateLoad(builder.getPtrTy(), var->addr, id->name + "_reborrow");
		return var->addr;
	}

	// 2. Array Index (e.g., `lstHuman[3]`)
	if (auto* aie = dynamic_cast<ArrayIndexExpr*>(e)) {
		if (!aie->array || aie->indices.empty()) return nullptr;
		llvm::Value* currentPtr = nullptr;
		llvm::Type* currentTy = nullptr;

		if (auto* id = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			VarInfo* var = lookupVar(id->name);
			if (!var) {
				error("Undefined variable '" + id->name + "'");
				return nullptr;
			}
			currentPtr = var->addr;
			currentTy = var->type;
		} else {
			currentPtr = emitRef(aie->array.get());
		}

		if (!currentPtr || !currentTy) return nullptr;

		for (size_t i = 0; i < aie->indices.size(); ++i) {
			llvm::Value* idx = emit(aie->indices[i].get());
			if (!idx) return nullptr;
			idx = coerce(idx, builder.getInt64Ty());

			if (currentTy->isArrayTy()) {
				llvm::ArrayType* arrTy = llvm::cast<llvm::ArrayType>(currentTy);
				currentPtr = builder.CreateInBoundsGEP(arrTy, currentPtr, { builder.getInt64(0), idx }, "array_idx_ptr");
				currentTy = arrTy->getElementType();
			} else {
				currentPtr = builder.CreateInBoundsGEP(builder.getInt8Ty(), currentPtr, { idx }, "ptr_idx_ptr");
			}
		}
		return currentPtr; // Returns the pointer to the element without loading it
	}

	// 3. Member Access (e.g., `human.age`)
	if (auto* mae = dynamic_cast<MemberAccessExpr*>(e)) {
		llvm::Value* addr = nullptr;
		llvm::Type* ty = nullptr;
		if (emitFieldAddr(*mae, addr, ty)) {
			return addr;
		}
		return nullptr;
	}

	error("Cannot borrow non-lvalue expression");
	return nullptr;
}

//==========================================================================//
//  3. Helpers                                                              //
//==========================================================================//
void CodeGenerator::error(const std::string& msg) {
	++errorCount;
	std::cerr << "Codegen Error: " << msg << "\n";
}

bool CodeGenerator::isUnsignedExpr(Expression* e) {
	if (!e) return false;

	if (auto* id = dynamic_cast<IdentifierExpr*>(e)) {
		VarInfo* var = lookupVar(id->name);
		if (var) return var->isUnsigned;
		return false;
	}

	if (auto* castExpr = dynamic_cast<CastExpr*>(e)) {
		DataType dt = castExpr->castType.type;
		return dt == DataType::U8 || dt == DataType::U16 ||
			dt == DataType::U32 || dt == DataType::U64 || dt == DataType::UInt;
	}

	if (auto* binExpr = dynamic_cast<BinaryExpr*>(e)) {
		return isUnsignedExpr(binExpr->left.get()) || isUnsignedExpr(binExpr->right.get());
	}

	if (auto* pf = dynamic_cast<PostfixExpr*>(e)) return isUnsignedExpr(pf->expr.get());

	if (auto* aie = dynamic_cast<ArrayIndexExpr*>(e)) {
		if (auto* base = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			if (VarInfo* var = lookupVar(base->name)) return var->isUnsigned;
		}
		return false;
	}

	if (auto* call = dynamic_cast<CallExpr*>(e)) {
		if (auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
			auto it = fnSigns.find(id->name == "Main" ? "main" : id->name);
			if (it != fnSigns.end()) return it->second.retUnsigned;
		}
		return false;
	}

	if (auto* ma = dynamic_cast<MemberAccessExpr*>(e)) {
		Expression* obj = ma->object.get();
		while (auto* aie = dynamic_cast<ArrayIndexExpr*>(obj)) obj = aie->array.get();

		if (auto* id = dynamic_cast<IdentifierExpr*>(obj)) {
			if (VarInfo* var = lookupVar(id->name)) {
				llvm::Type* t = var->type;
				while (t->isArrayTy()) t = llvm::cast<llvm::ArrayType>(t)->getElementType();
				llvm::StructType* st = var->structType ? var->structType : llvm::dyn_cast<llvm::StructType>(t);
				if (st) {
					auto it = structs.find(st->getName().str());
					if (it != structs.end()) {
						const auto& names = it->second.fieldNames;
						auto f = std::find(names.begin(), names.end(), ma->memberName);
						size_t idx = static_cast<size_t>(f - names.begin());
						if (f != names.end() && idx < it->second.fieldIsUnsigned.size())
							return it->second.fieldIsUnsigned[idx];
					}
				}
			}
		}
		return false;
	}

	return false;
}

// Parses an integer literal into sign + magnitude. Handles '_' separators,
// a leading '-', and 0x / 0o / 0b prefixes.
bool CodeGenerator::parseIntLiteral(LiteralExpr& lit, bool& neg, unsigned long long& mag) {
	std::string text = tokenText(lit.lit);
	text.erase(std::remove(text.begin(), text.end(), '_'), text.end());

	neg = false;
	size_t skip = 0;
	if (!text.empty() && text[0] == '-') { neg = true; skip = 1; }

	int base = 10;
	switch (lit.base) {
		case NumericBase::Hexadecimal: base = 16; skip += 2; break;
		case NumericBase::Octal:       base = 8;  skip += 2; break;
		case NumericBase::Binary:      base = 2;  skip += 2; break;
		default: break;
	}
	try {
		mag = std::stoull(text.substr(skip), nullptr, base);
	} catch (...) {
		error("Invalid integer literal '" + text + "'");
		return false;
	}
	if (mag == 0) neg = false; // -0 is just 0
	return true;
}

// Compile-time value of an integer constant expression: `5`, `-5`, `-(-5)`.
// Returns false if `e` is not such a constant.
bool CodeGenerator::evalConstInt(Expression* e, bool& neg, unsigned long long& mag) {
	if (auto* lit = dynamic_cast<LiteralExpr*>(e)) {
		if (lit->kind != LiteralKind::Int) return false;
		return parseIntLiteral(*lit, neg, mag);
	}
	if (auto* un = dynamic_cast<UnaryExpr*>(e); un && un->op == UnaryOp::Negate) {
		if (!evalConstInt(un->expr.get(), neg, mag)) return false;
		if (mag != 0) neg = !neg;
		return true;
	}
	return false;
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

// A pointer-sized stack slot in the entry block, initialised to null right
// there. Because the null store sits at function entry, every path through the
// function sees a valid (null or real) pointer when the cleanup code loads it.
llvm::AllocaInst* CodeGenerator::createHeapSlot(const std::string& name) {
	llvm::BasicBlock& entry = currentFn->getEntryBlock();
	llvm::IRBuilder<> tmp(&entry, entry.begin());
	llvm::AllocaInst* slot = tmp.CreateAlloca(builder.getPtrTy(), nullptr, name);
	tmp.CreateStore(llvm::ConstantPointerNull::get(builder.getPtrTy()), slot);
	return slot;
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
		if (var->type->isPointerTy() && var->structType != nullptr) {
			objPtr   = builder.CreateLoad(builder.getPtrTy(), var->addr, objId->name + "_load");
			structTy = var->structType;
		} else {
			objPtr   = var->addr;
			structTy = var->structType ? var->structType : llvm::dyn_cast<llvm::StructType>(var->type);
		}
	}
	else if (auto* aie = dynamic_cast<ArrayIndexExpr*>(ma.object.get())) {
		objPtr = emitLValue(aie); // Get the pointer to the struct element

		if (auto* baseId = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
			VarInfo* var = lookupVar(baseId->name);
			if (var && var->type->isArrayTy()) {
				llvm::ArrayType* arrTy = llvm::cast<llvm::ArrayType>(var->type);
				llvm::Type* elemTy = arrTy->getElementType();
				structTy = llvm::dyn_cast<llvm::StructType>(elemTy);
			}
		}
	}
	else {
		// Fallback for general expressions
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

// Emits `if (*slot != null && *slot != keep) { free(*slot); *slot = null; }`
// for every slot. `keep` (may be nullptr) is a pointer that must survive, e.g.
// the string being returned from the function.
void CodeGenerator::emitCleanups(const std::vector<llvm::Value*>& slots, llvm::Value* keep) {
	if (slots.empty() || blockTerminated()) return;

	auto freeFn = module->getOrInsertFunction(
			"free", llvm::FunctionType::get(builder.getVoidTy(), { builder.getPtrTy() }, false));
	llvm::Value* nullPtr = llvm::ConstantPointerNull::get(builder.getPtrTy());

	// Newest allocation first.
	for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
		llvm::Value* slot = *it;
		if (!slot) continue;

		llvm::Value* heapPtr = builder.CreateLoad(builder.getPtrTy(), slot, "heap_ptr");

		llvm::Value* shouldFree = builder.CreateIsNotNull(heapPtr, "is_not_null");
		if (keep && keep->getType()->isPointerTy()) {
			llvm::Value* notKept = builder.CreateICmpNE(heapPtr, keep, "not_kept");
			shouldFree = builder.CreateAnd(shouldFree, notKept, "should_free");
		}

		llvm::BasicBlock* freeBlock = llvm::BasicBlock::Create(context, "do_free", currentFn);
		llvm::BasicBlock* contBlock = llvm::BasicBlock::Create(context, "cont_free", currentFn);
		builder.CreateCondBr(shouldFree, freeBlock, contBlock);

		builder.SetInsertPoint(freeBlock);
		builder.CreateCall(freeFn, { heapPtr });
		builder.CreateStore(nullPtr, slot); // avoid double free if the slot is reused
		builder.CreateBr(contBlock);

		builder.SetInsertPoint(contBlock);
	}
}

void CodeGenerator::popScope() {
	if (scopeCleanups.empty()) return;

	auto currentScope = std::move(scopeCleanups.back());
	scopeCleanups.pop_back();

	emitCleanups(currentScope, nullptr);
}

void CodeGenerator::registerCleanup(llvm::Value* slot) {
	if (!scopeCleanups.empty() && slot) {
		scopeCleanups.back().push_back(slot);
	}
}

llvm::Value* CodeGenerator::emitUnsignedValue(Expression* e, llvm::Type* ty, const std::string& what) {
	if (!e || !ty) return nullptr;
	if (!ty->isIntegerTy()) return emit(e);

	bool neg = false;
	unsigned long long mag = 0;
	if (evalConstInt(e, neg, mag)) {
		if (neg) { error("Negative constant for unsigned " + what); return nullptr; }
		unsigned bits = ty->getIntegerBitWidth();
		if (bits < 64 && mag > ((1ULL << bits) - 1)) {
			error("Value " + std::to_string(mag) + " is out of range for unsigned " + what);
			return nullptr;
		}
		return llvm::ConstantInt::get(ty, mag, false);
	}

	llvm::Value* v = emit(e);
	if (!v) return nullptr;

	if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1) &&
			!llvm::isa<llvm::Constant>(v) && !isUnsignedExpr(e))
		emitNegativeCheck(v, what);
	return v;
}

void CodeGenerator::emitNegativeCheck(llvm::Value* v, const std::string& what) {
	llvm::Value* isNeg = builder.CreateICmpSLT(v, llvm::ConstantInt::get(v->getType(), 0), "isneg");

	auto* failBB = llvm::BasicBlock::Create(context, "neg_fail", currentFn);
	auto* okBB   = llvm::BasicBlock::Create(context, "neg_ok",   currentFn);
	builder.CreateCondBr(isNeg, failBB, okBB);

	builder.SetInsertPoint(failBB);
	std::string msg = "Runtime Error: negative value assigned to unsigned " + what + "\n";
	auto writeFn = module->getOrInsertFunction("write", llvm::FunctionType::get(
			builder.getInt64Ty(), { builder.getInt32Ty(), builder.getPtrTy(), builder.getInt64Ty() }, false));
	auto exitFn = module->getOrInsertFunction("exit", llvm::FunctionType::get(
			builder.getVoidTy(), { builder.getInt32Ty() }, false));
	llvm::Value* str = builder.CreateGlobalString(msg, "neg_msg");
	builder.CreateCall(writeFn, { builder.getInt32(2), str, builder.getInt64(msg.size()) });
	builder.CreateCall(exitFn, { builder.getInt32(1) });
	builder.CreateUnreachable();

	builder.SetInsertPoint(okBB);
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
			bool neg = false;
			unsigned long long mag = 0;
			if (!parseIntLiteral(node, neg, mag)) { lastValue = nullptr; break; }

			if (neg) {
				long long sv = -static_cast<long long>(mag);
				llvm::Type* ty = (sv >= INT32_MIN) ? builder.getInt32Ty() : builder.getInt64Ty();
				lastValue = llvm::ConstantInt::get(ty, static_cast<uint64_t>(sv), /*isSigned=*/true);
			} else {
				llvm::Type* ty = (mag > 0x7FFFFFFFULL) ? builder.getInt64Ty() : builder.getInt32Ty();
				lastValue = llvm::ConstantInt::get(ty, mag, false);
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

llvm::Type* CodeGenerator::getExprType(Expression* e) {
	if (!e) return nullptr;
	if (auto* sce = dynamic_cast<StructCreateExpr*>(e)) {
		if (auto* id = dynamic_cast<IdentifierExpr*>(sce->structObj.get())) {
			auto it = structs.find(id->name);
			if (it != structs.end()) return it->second.type;
		}
	}
	if (auto* ale = dynamic_cast<ArrayLiteralExpr*>(e)) {
		if (!ale->elements.empty()) {
			llvm::Type* innerElemTy = getExprType(ale->elements[0].get());
			if (innerElemTy) {
				return llvm::ArrayType::get(innerElemTy, ale->elements.size());
			}
		}
	}
	llvm::Value* v = emit(e);
	return v ? v->getType() : nullptr;
}

void CodeGenerator::visit(ArrayLiteralExpr& node) {
	bool ctx = unsignedCtx;
	unsignedCtx = false;
	lastValue = nullptr;
	if (node.elements.empty()) {
		error("Empty array literal not supported without explicit type");
		return;
	}

	// Correctly resolve the expected element type (struct or nested array)
	llvm::Type* elemTy = getExprType(node.elements[0].get());
	if (!elemTy) {
		llvm::Value* firstVal = emit(node.elements[0].get());
		if (!firstVal) return;
		elemTy = firstVal->getType();
	}

	size_t numElements = node.elements.size();
	llvm::ArrayType* arrayTy = llvm::ArrayType::get(elemTy, numElements);
	if (!currentFn) {
		error("Array literal outside of a function");
		return;
	}

	llvm::AllocaInst* arrayAlloca = createEntryAlloca(currentFn, arrayTy, "array_lit");

	for (size_t i = 0; i < numElements; ++i) {
		llvm::Value* val = nullptr;
		Expression* el = node.elements[i].get();
		if (ctx) {
			if (dynamic_cast<ArrayLiteralExpr*>(el)) { unsignedCtx = true; val = emit(el); }
			else val = emitUnsignedValue(el, elemTy, "array element");
		} else {
			val = emit(el);
		}
		if (!val) return;

		// If the element type is a struct or array, load it from its temporary pointer
		if ((elemTy->isStructTy() || elemTy->isArrayTy()) && val->getType()->isPointerTy()) {
			val = builder.CreateLoad(elemTy, val, "elem_load");
		} else {
			val = coerce(val, elemTy);
		}

		llvm::Value* elemPtr = builder.CreateInBoundsGEP(
				arrayTy,
				arrayAlloca,
				{ builder.getInt64(0), builder.getInt64(i) },
				"array_elem_ptr"
				);
		builder.CreateStore(val, elemPtr);
	}

	lastValue = arrayAlloca;
}

void CodeGenerator::visit(ArrayIndexExpr& node) {
	lastValue = nullptr;
	if (!node.array || node.indices.empty()) return;

	llvm::Value* currentPtr = nullptr;
	llvm::Type* currentTy = nullptr;

	// Get the base pointer and type of the array
	if (auto* id = dynamic_cast<IdentifierExpr*>(node.array.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		currentPtr = var->addr; // This is the alloca pointer ([N x T]*)
		currentTy = var->type;  // This is the array type ([N x T])
	} else {
		currentPtr = emitRef(node.array.get());
	}

	if (!currentPtr || !currentTy) {
		error("Invalid array expression");
		return;
	}

	// Iterate through each index dimension
	for (size_t i = 0; i < node.indices.size(); ++i) {
		llvm::Value* idx = emit(node.indices[i].get());
		if (!idx) return;
		idx = coerce(idx, builder.getInt64Ty());

		if (currentTy->isArrayTy()) {
			llvm::ArrayType* arrTy = llvm::cast<llvm::ArrayType>(currentTy);
			// GEP for array alloca: requires {0, idx}
			currentPtr = builder.CreateInBoundsGEP(arrTy, currentPtr, { builder.getInt64(0), idx }, "array_idx");
			currentTy = arrTy->getElementType();
		} else if (currentPtr->getType()->isPointerTy()) {
			currentPtr = builder.CreateInBoundsGEP(currentTy, currentPtr, { idx }, "ptr_idx");
		} else {
			error("Indexing applied to non-array type");
			return;
		}
	}

	// Load the final value if it's a primitive/element, or return the pointer if it's a nested array
	if (currentTy->isArrayTy()) {
		lastValue = currentPtr;
	} else {
		lastValue = builder.CreateLoad(currentTy, currentPtr, "val_load");
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

	bool isUnsigned = node.type.isUnsignedInteger();

	llvm::Value* init = nullptr;
	if (node.expr) {
		if (node.type.isReference) {
			init = emitRef(node.expr.get());
		} else if (isUnsigned) {
			llvm::Type* want = mapType(node.type);
			if (want && want->isIntegerTy()) {
				init = emitUnsignedValue(node.expr.get(), want, "variable '" + name + "'");
			} else {
				unsignedCtx = dynamic_cast<ArrayLiteralExpr*>(node.expr.get()) != nullptr;
				init = emit(node.expr.get());
				unsignedCtx = false;
			}
		} else {
			init = emit(node.expr.get());
		}
	}
	if (node.expr && !init) { lastValue = nullptr; return; }

	llvm::Type* ty = (node.type.type == DataType::Void && init) ? init->getType() : mapType(node.type);
	if (!ty || ty->isVoidTy()) { error("Invalid type for variable '" + name + "'"); return; }


	llvm::Value* initVal = nullptr;
	if (init) {
		if ((ty->isStructTy() || ty->isArrayTy()) && init->getType()->isPointerTy()) {
			initVal = builder.CreateLoad(ty, init, "init_load");
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

	scopes.back()[name] = { slot, ty, st, pointee, isUnsigned };
	lastValue = initVal;
}

void CodeGenerator::visit(AssignExpr& node) {
	lastValue = nullptr;
	llvm::Value* targetPtr = nullptr;
	llvm::Type* targetType = nullptr;
	bool targetUnsigned = false;

	if (auto* id = dynamic_cast<IdentifierExpr*>(node.target.get())) {
		VarInfo* var = lookupVar(id->name);
		if (!var) {
			error("Undefined variable '" + id->name + "'");
			return;
		}
		targetUnsigned = var->isUnsigned;
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
		targetUnsigned = isUnsignedExpr(ma);
	}
	else {
		error("Only variables or structure fields can be assigned to");
		return;
	}

	llvm::Value* raw = (targetUnsigned && targetType->isIntegerTy())
		? emitUnsignedValue(node.value.get(), targetType, "variable")
		: emit(node.value.get());
	llvm::Value* v = coerce(raw, targetType);
	if (!v) {
		lastValue = nullptr;
		return;
	}

	builder.CreateStore(v, targetPtr);
	lastValue = v;
}

void CodeGenerator::visit(BorrowExpr& node) {
	lastValue = emitLValue(node.expr.get());
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
	bool isUnsigned = isUnsignedExpr(node.left.get()) || isUnsignedExpr(node.right.get());
	lastValue = (l && r) ? emitBinary(node.op, l, r, isUnsigned) : nullptr;
}

void CodeGenerator::visit(UnaryExpr& node) {
	// `-x` makes no sense for an unsigned variable.
	if (node.op == UnaryOp::Negate && isUnsignedExpr(node.expr.get())) {
		error("Cannot negate an unsigned value");
		lastValue = nullptr;
		return;
	}

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

		bool paramUnsigned = !paramDesc.type.isReference && paramDesc.type.isUnsignedInteger();
		scopes.back()[pname] = { slot, arg.getType(), st, pointee, paramUnsigned };
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

	if (!it->second.type->isOpaque()) return;

	StructInfo& info = it->second;
	info.fieldNames.clear();
	info.fieldIsRef.clear();
	info.fieldPointee.clear();
	info.fieldIsUnsigned.clear();

	std::vector<llvm::Type*> fieldTypes;
	for (const auto& f : node.attributes) {
		llvm::Type* ft = mapType(f.type);
		if (!ft || ft->isVoidTy()) { error("Invalid type for field '" + f.name + "'"); return; }
		fieldTypes.push_back(ft);
		info.fieldNames.push_back(f.name);
		info.fieldIsRef.push_back(f.type.isReference);
		info.fieldPointee.push_back(pointeeOf(f.type));
		info.fieldIsUnsigned.push_back(!f.type.isReference && f.type.isUnsignedInteger());
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

	llvm::AllocaInst* structAlloca = createEntryAlloca(currentFn, llvmStructTy, id->name + "_tmp");
	builder.CreateStore(llvm::Constant::getNullValue(llvmStructTy), structAlloca);

	for (size_t i = 0; i < node.arguments.size(); ++i) {
		llvm::Type* fieldTy = llvmStructTy->getElementType(i);
		bool isRef = i < info.fieldIsRef.size() && info.fieldIsRef[i];
		bool isUns = i < info.fieldIsUnsigned.size() && info.fieldIsUnsigned[i] && fieldTy->isIntegerTy();

		llvm::Value* argVal = isUns
			? emitUnsignedValue(node.arguments[i].get(), fieldTy, "field '" + info.fieldNames[i] + "'")
			: (isRef ? emitRef(node.arguments[i].get()) : emit(node.arguments[i].get()));
		if (!argVal) return;

		argVal = coerce(argVal, fieldTy);
		if (!argVal) return;

		llvm::Value* fieldPtr = builder.CreateStructGEP(llvmStructTy, structAlloca, i,
				id->name + "_field_" + std::to_string(i));
		builder.CreateStore(argVal, fieldPtr);
	}

	lastValue = structAlloca;
}

void CodeGenerator::visit(MemberAccessExpr& node) {
	lastValue = nullptr;

	if (node.memberName == "length") {
		llvm::Type* arrTy = nullptr;

		// 1. Check if the object is a direct array identifier (e.g., lstHuman.length)
		if (auto* id = dynamic_cast<IdentifierExpr*>(node.object.get())) {
			VarInfo* var = lookupVar(id->name);
			if (var) arrTy = var->type;
		}
		// 2. Check if the object is an array index expression (e.g., lstHuman[cptX].length)
		else if (auto* aie = dynamic_cast<ArrayIndexExpr*>(node.object.get())) {
			if (auto* baseId = dynamic_cast<IdentifierExpr*>(aie->array.get())) {
				VarInfo* var = lookupVar(baseId->name);
				if (var && var->type->isArrayTy()) {
					arrTy = var->type;
					// Peel back one array dimension for each index accessed
					for (size_t i = 0; i < aie->indices.size(); ++i) {
						if (arrTy->isArrayTy()) {
							arrTy = llvm::cast<llvm::ArrayType>(arrTy)->getElementType();
						}
					}
				}
			}
		}

		// If we successfully resolved a multi-dimensional or 1D array type, return its compile-time size
		if (arrTy && arrTy->isArrayTy()) {
			uint64_t len = llvm::cast<llvm::ArrayType>(arrTy)->getNumElements();
			lastValue = builder.getInt32(static_cast<uint32_t>(len));
			return;
		}

		// 3. Fallback: Check if it's a string type (emits a runtime strlen call)
		llvm::Value* objVal = emit(node.object.get());
		if (objVal && objVal->getType()->isPointerTy()) {
			auto sizeTy = builder.getInt64Ty();
			auto strlenFn = module->getOrInsertFunction("strlen", llvm::FunctionType::get(sizeTy, { builder.getPtrTy() }, false));
			llvm::Value* strPtr = builder.CreatePointerCast(objVal, builder.getPtrTy(), "str_cast");
			llvm::Value* len64 = builder.CreateCall(strlenFn, { strPtr }, "strlen_tmp");
			lastValue = builder.CreateTrunc(len64, builder.getInt32Ty(), "len_i32");
			return;
		}
	}

	// 4. Fallback to regular structure field access
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

void CodeGenerator::visit(ImportExpr&) { /* handled by the compiler pipeline */ }
