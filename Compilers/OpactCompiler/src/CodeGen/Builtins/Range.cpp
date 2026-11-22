//==========================================================================//
//  Range.cpp  -  Range(end) / Range(start, end[, step])                    //
//==========================================================================//
#include "../CodeGenerator.hpp"
#include "../StringUtil.hpp"
#include <cstdint>

void CodeGenerator::registerRangeBuiltins() {
	registerBuiltin("Range", [this](CallExpr& c) { return emitRange(c); });
}

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
	bool allConst = true;
	for (size_t i = 0; i < n; ++i) {
		if (!constI32(call.arguments[i].get(), v[i])) {
			allConst = false;
			break;
		}
	}

	if (allConst) {
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

	// Runtime bounds: emit a "Range" struct { i32 start, i32 end } for ForExpr.
	llvm::Type* i32 = builder.getInt32Ty();
	llvm::Value* startVal = llvm::ConstantInt::get(i32, 0);
	llvm::Value* endVal = nullptr;

	auto evalBound = [&](Expression* expr) -> llvm::Value* {
		llvm::Value* raw = emit(expr);
		if (!raw) return nullptr;
		return coerce(raw, i32);
	};

	if (n == 1) {
		endVal = evalBound(call.arguments[0].get());
		if (!endVal) return nullptr;
	} else {
		startVal = evalBound(call.arguments[0].get());
		endVal = evalBound(call.arguments[1].get());
		if (!startVal || !endVal) return nullptr;
	}

	llvm::StructType* rangeTy = llvm::StructType::getTypeByName(context, "Range");
	if (!rangeTy) {
		rangeTy = llvm::StructType::create(context, { i32, i32 }, "Range");
	}

	llvm::AllocaInst* slot = createEntryAlloca(currentFn, rangeTy, "range_dyn");
	builder.CreateStore(startVal, builder.CreateStructGEP(rangeTy, slot, 0, "range_start_ptr"));
	builder.CreateStore(endVal, builder.CreateStructGEP(rangeTy, slot, 1, "range_end_ptr"));
	return slot;
}
