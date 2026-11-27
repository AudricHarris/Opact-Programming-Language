//==========================================================================//
//  Libc.cpp  -  thin wrappers over C library functions                     //
//  registerExternal(userName, cName, returnType, {paramTypes}) declares the C//
//  function lazily and coerces the arguments for you.                      //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../StringUtil.hpp"
#include <cstdint>

void CodeGenerator::registerLibcBuiltins() {
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Type* ptr = builder.getPtrTy();
	llvm::Type* voidTy = builder.getVoidTy();

	registerExternal("Exit", "exit", voidTy, { i32 });
	registerExternal("Puts", "puts", i32,    { ptr });
	registerExternal("Abs",  "abs",  i32,    { i32 });
}
