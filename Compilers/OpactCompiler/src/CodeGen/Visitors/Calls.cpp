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

	if (auto* enumGet = dynamic_cast<EnumGetExpr*>(node.callee.get())) {
		if (!currentFn) {
			error("Enum variant call outside of a function");
			return;
		}
		if (auto* targetId = dynamic_cast<IdentifierExpr*>(enumGet->target.get())) {
			auto eit = enums.find(targetId->name);
			if (eit != enums.end() && eit->second.storageType) {
				const EnumInfo& einfo = eit->second;
				const EnumVariantInfo* vinfo = findEnumVariant(einfo, enumGet->fieldName);
				if (!vinfo) {
					error("Unknown variant '" + enumGet->fieldName + "' for enum '" + targetId->name + "'");
					return;
				}
				if (vinfo->payloadTypes.size() != node.arguments.size()) {
					error("Variant '" + targetId->name + "::" + enumGet->fieldName + "' expects " +
							std::to_string(vinfo->payloadTypes.size()) + " argument(s), got " +
							std::to_string(node.arguments.size()));
					return;
				}

				llvm::AllocaInst* enumAlloca = createEntryAlloca(
						currentFn, einfo.storageType, targetId->name + "_tmp");
				builder.CreateStore(llvm::Constant::getNullValue(einfo.storageType), enumAlloca);

				llvm::Value* tagPtr = builder.CreateStructGEP(
						einfo.storageType, enumAlloca, 0, "enum_tag_ptr");
				builder.CreateStore(builder.getInt32(vinfo->tag), tagPtr);

				if (!node.arguments.empty()) {
					llvm::Value* payloadArrPtr = builder.CreateStructGEP(
							einfo.storageType, enumAlloca, 1, "enum_payload_ptr");
					llvm::Type* payloadArrTy = llvm::ArrayType::get(builder.getInt8Ty(), einfo.payloadBytes);

					for (size_t i = 0; i < node.arguments.size(); ++i) {
						if (!node.arguments[i]) continue;
						llvm::Value* argVal = emit(node.arguments[i].get());
						if (!argVal) return;

						llvm::Type* targetTy = vinfo->payloadTypes[i];
						if ((targetTy->isStructTy() || targetTy->isArrayTy()) && argVal->getType()->isPointerTy()) {
							argVal = builder.CreateLoad(targetTy, argVal, "enum_arg_load");
						}
						argVal = coerce(argVal, targetTy);
						if (!argVal) return;

						llvm::Value* bytePtr = builder.CreateConstGEP2_64(
								payloadArrTy, payloadArrPtr, 0, vinfo->payloadOffsets[i],
								"payload_off_" + std::to_string(i));
						llvm::Value* typedPtr = builder.CreatePointerCast(
								bytePtr, builder.getPtrTy(), "payload_typed_ptr");
						builder.CreateStore(argVal, typedPtr);
					}
				}

				lastValue = enumAlloca;
				return;
			}
		}
	}

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
