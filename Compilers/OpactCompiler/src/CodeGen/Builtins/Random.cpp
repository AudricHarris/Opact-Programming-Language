//==========================================================================//
//  Random.cpp  -  Random() -> f64 in [0, 1)                                //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../StringUtil.hpp"
#include <cstdint>

void CodeGenerator::registerRandomBuiltins() {
	registerBuiltin("Random", [this](CallExpr& c) { return emitRandom(c); });
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
