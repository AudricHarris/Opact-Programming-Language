//==========================================================================//
//  Declarations.cpp  -  functions, structures, imports                     //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../DynArray.hpp"
#include "../StringUtil.hpp"
#include <llvm/IR/Instructions.h>
#include <algorithm>
#include <cstdint>

using namespace codegen_detail;

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

void CodeGenerator::visit(ImportExpr&) { /* handled by the compiler pipeline */ }
