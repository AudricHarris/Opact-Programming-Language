//==========================================================================//
//  Read.cpp  -  Read() -> line from stdin (str)                            //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../StringUtil.hpp"
#include <cstdint>

void CodeGenerator::registerReadBuiltins() {
	registerBuiltin("Read", [this](CallExpr& c) { return emitRead(c); });
}

llvm::Value* CodeGenerator::emitRead(CallExpr& call) {
	if (!currentFn) { error("Read used outside of a function"); return nullptr; }
	if (!call.arguments.empty()) { error("Read takes no arguments"); return nullptr; }

	llvm::Type* i8 = builder.getInt8Ty();
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* i64 = builder.getInt64Ty();
	llvm::Type* ptr = builder.getPtrTy();

	auto fgetsFn = module->getOrInsertFunction("fgets",
			llvm::FunctionType::get(ptr, { ptr, i32, ptr }, false));
	auto strcspnFn = module->getOrInsertFunction("strcspn",
			llvm::FunctionType::get(i64, { ptr, ptr }, false));

	auto stdinGlobal = module->getNamedGlobal("stdin");
	if (!stdinGlobal) {
		stdinGlobal = new llvm::GlobalVariable(
				*module, ptr, false, llvm::GlobalValue::ExternalLinkage,
				nullptr, "stdin"
				);
	}

	const unsigned bufferSize = 256;
	llvm::ArrayType* bufferType = llvm::ArrayType::get(i8, bufferSize);
	llvm::AllocaInst* bufferAlloca = createEntryAlloca(currentFn, bufferType, "read_buf");

	llvm::Value* zero = llvm::ConstantInt::get(i32, 0);
	llvm::Value* bufPtr = builder.CreateInBoundsGEP(bufferType, bufferAlloca, { zero, zero }, "buf_ptr");

	llvm::Value* stdinVal = builder.CreateLoad(ptr, stdinGlobal, "stdin_val");

	llvm::Value* sizeArg = llvm::ConstantInt::get(i32, bufferSize);
	builder.CreateCall(fgetsFn, { bufPtr, sizeArg, stdinVal });

	// Supprime le '\n' final si présent : buf[strcspn(buf, "\n")] = '\0'
	llvm::Value* nlPattern = builder.CreateGlobalString("\n", "nl_str");
	llvm::Value* nlPos = builder.CreateCall(strcspnFn, { bufPtr, nlPattern }, "nl_pos");
	llvm::Value* nlPtr = builder.CreateInBoundsGEP(i8, bufPtr, { nlPos }, "nl_ptr");
	builder.CreateStore(llvm::ConstantInt::get(i8, 0), nlPtr);

	return bufPtr;
}
