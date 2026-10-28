#include "TypeChecker.hpp"
#include "../Parser/Ast.hpp"
#include "../Parser/CompilerPipeline.hpp"
#include "Symbol/FunctionSymbol.hpp"
#include "Symbol/StructSymbol.hpp"
#include "Symbol/VarSymbol.hpp"
#include "Types/ArrayType.hpp"
#include "Types/PrimitiveType.hpp"
#include "Types/ReferenceType.hpp"
#include "Types/StructType.hpp"
#include "Types/Type.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {
	// The ONLY place that touches Token's text member. If your Token uses a
	// different field name (value, text, str, ...), change it here.
	std::string tokenText(const Token& t) { return t.getWord(); }

	std::shared_ptr<Type> prim(BuiltinType t) { return std::make_shared<PrimitiveType>(t); }

	bool isPrim(const std::shared_ptr<Type>& t, BuiltinType b) {
		return t && t->getKind() == TypeKind::Primitive &&
			static_cast<const PrimitiveType*>(t.get())->getBuiltinType() == b;
	}

	bool isNumeric(const std::shared_ptr<Type>& t) {
		return isPrim(t, BuiltinType::Int) || isPrim(t, BuiltinType::Float);
	}

	// `&T` / `&mut T` -> `T` (references are auto-dereferenced when used as values)
	std::shared_ptr<Type> stripRef(const std::shared_ptr<Type>& t) {
		if (t && t->getKind() == TypeKind::Reference)
			return static_cast<const ReferenceType*>(t.get())->getInnerType();
		return t;
	}

	bool isRefType(const std::shared_ptr<Type>& t) {
		return t && t->getKind() == TypeKind::Reference;
	}

	bool carriesReference(const std::shared_ptr<Type>& t) {
		if (!t) return false;
		if (isRefType(t)) return true;
		if (t->getKind() == TypeKind::Struct) {
			auto* st = static_cast<StructType*>(t.get());
			for (const auto& f : st->getOrderedFields())
				if (carriesReference(f.type)) return true;
		}
		return false;
	}

	IdentifierExpr* rootIdent(Expression* e) {
		while (e) {
			if (auto* id = dynamic_cast<IdentifierExpr*>(e)) return id;
			if (auto* ma = dynamic_cast<MemberAccessExpr*>(e)) { e = ma->object.get(); continue; }
			return nullptr;
		}
		return nullptr;
	}
} // namespace

TypeChecker::TypeChecker(CompilerPipeline* pipeline, const std::string& filePath)
	: pipeline(pipeline), currentFilePath(filePath), currentType(prim(BuiltinType::Void)) 
{
	this->declareBuiltins();
}

void TypeChecker::ShowErrorCount()
{
	std::cerr << "Total error Count after TypeChecker : " << this->errorCount_ << "\n";
}

// Handling Built in functions 
struct BuiltinParam {
	const char* name;
	DataType type;
};

struct BuiltinInfo {
	const char* name;
	DataType returnType;
	std::initializer_list<BuiltinParam> params;
};

void TypeChecker::declareBuiltins() {
	std::initializer_list<BuiltinInfo> builtins = {
		{ "Printf", DataType::Void, { {"Text", DataType::Str} } },
		{ "Println", DataType::Void, { {"Text", DataType::Str} } },
	};

	persistentBuiltins_.reserve(builtins.size());

	for (const auto& info : builtins) {
		auto& fn = persistentBuiltins_.emplace_back();
		fn.name = info.name;
		ExprPtr body;
		fn.body = std::move(body);
		fn.returnType.type = info.returnType;

		for (const auto& p : info.params) {
			TypeDesc pType;
			pType.type = p.type;
			fn.params.emplace_back(p.name, std::move(pType), false, false, std::nullopt);
		}

		this->declareFunction(fn);
	}
}

static std::string getSourceLine(const std::string& filePath, size_t lineNumber) {
	std::ifstream file(filePath);
	std::string line;
	size_t current = 1;
	while (std::getline(file, line)) {
		if (current == lineNumber) return line;
		current++;
	}
	return "";
}

void TypeChecker::reportError(size_t line, size_t col, const std::string& primaryMsg, const std::string& note) {
	++errorCount_;
	this->correctCode = false;

	std::cerr << "\033[1;31mOpacity Error\033[0m: " << primaryMsg << "\n";
	if (line > 0) {
		std::cerr << "  --> " << currentFilePath << ":" << line << ":" << col << "\n";

		std::string sourceLine = getSourceLine(currentFilePath, line);
		if (!sourceLine.empty()) {
			std::cerr << "   |\n";
			std::cerr << line << " ==> " << sourceLine << "\n";

			std::string indent(col > 0 ? col - 1 : 0, ' ');
			std::cerr << "   | " << indent << "\033[1;31m^\033[0m\n";
		}
	}

	if (!note.empty()) {
		std::cerr << "   |     \033[1;36mNOTE:\033[0m " << note << "\n";
	}
	std::cerr << "\n";
}

void TypeChecker::reportError(const std::string& message) {
	reportError(0, 0, message, "");
}

std::string TypeChecker::escapingLocal(Expression& e) {
	auto danglingOf = [&](IdentifierExpr* id) -> std::string {
		if (!id) return "";
		auto sym = scopeManager.lookup(id->name);
		if (!sym) return "";
		auto it = localInfo_.find(sym.get());
		return it == localInfo_.end() ? "" : it->second.dangling;
	};

	if (auto* b = dynamic_cast<BorrowExpr*>(&e)) {
		if (!b->expr) return "";
		auto* id = rootIdent(b->expr.get());
		if (!id) return "";
		auto sym = scopeManager.lookup(id->name);
		if (!sym || !localInfo_.count(sym.get())) return "";
		if (isRefType(sym->getType())) return danglingOf(id);
		return id->name; // borrowing a local value
	}
	if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) return danglingOf(id);
	if (auto* ma = dynamic_cast<MemberAccessExpr*>(&e)) return danglingOf(rootIdent(ma));

	if (auto* sc = dynamic_cast<StructCreateExpr*>(&e)) {
		for (auto& arg : sc->arguments) {
			if (!arg) continue;
			std::string s = escapingLocal(*arg);
			if (!s.empty()) return s;
		}
	}

	if (auto* call = dynamic_cast<CallExpr*>(&e)) {
		auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
		if (!callee) return "";
		auto fsym = scopeManager.lookup(callee->name);
		if (!fsym || fsym->getType()->getKind() != TypeKind::Function) return "";

		auto* fn = static_cast<FunctionType*>(fsym->getType().get());
		// A function returning plain data (int, str, ...) cannot leak a borrow.
		if (!carriesReference(fn->getReturnType())) return "";

		const auto& params = fn->getParamTypes();
		for (size_t i = 0; i < call->arguments.size() && i < params.size(); ++i) {
			if (!call->arguments[i] || !carriesReference(params[i])) continue;
			std::string s = escapingLocal(*call->arguments[i]);
			if (!s.empty()) return s;
		}
	}

	return "";
}

// ---------- Type conversion helpers ----------

std::shared_ptr<Type> TypeChecker::fromDesc(const TypeDesc& d) {
	std::shared_ptr<Type> baseType = prim(BuiltinType::Void);
	switch (d.type) {
		case DataType::Int:   baseType = prim(BuiltinType::Int);   break;
		case DataType::Float: baseType = prim(BuiltinType::Float); break;
		case DataType::Bool:  baseType = prim(BuiltinType::Bool);  break;
		case DataType::Char:  baseType = prim(BuiltinType::Char);  break;
		case DataType::Str:   baseType = prim(BuiltinType::Str);   break;
		case DataType::Void:  baseType = prim(BuiltinType::Void);  break;
		case DataType::Custom: {
			std::string name = d.customTypeName;
			if (name.empty() && d.customName) name = tokenText(*d.customName);
			auto sym = scopeManager.lookup(name);

			if (!sym || sym->getKind() != SymbolKind::Struct) {
				reportError("Unknown type: " + name);
				return prim(BuiltinType::Void);
			}
			baseType = sym->getType();
			break;
		}
	}

	// Wrap in ArrayType if dimensions are present (e.g. i32[5])
	if (!d.dim.empty()) {
		for (auto it = d.dim.rbegin(); it != d.dim.rend(); ++it) {
			baseType = std::make_shared<ArrayType>(baseType, *it);
		}
	}

	if (d.isReference) {
		return std::make_shared<ReferenceType>(baseType, d.isMutable);
	}

	return baseType;
}

TypeDesc TypeChecker::toDesc(const std::shared_ptr<Type>& type) const {
	TypeDesc d;
	if (!type) return d;
	switch (type->getKind()) {
		case TypeKind::Primitive:
			switch (static_cast<const PrimitiveType*>(type.get())->getBuiltinType()) {
				case BuiltinType::Int:   d.type = DataType::Int;   break;
				case BuiltinType::Float: d.type = DataType::Float; break;
				case BuiltinType::Bool:  d.type = DataType::Bool;  break;
				case BuiltinType::Char:  d.type = DataType::Char;  break;
				case BuiltinType::Str:   d.type = DataType::Str;   break;
				case BuiltinType::Void:  d.type = DataType::Void;  break;
			}
			break;
		case TypeKind::Struct:
			d.type = DataType::Custom;
			d.customTypeName = static_cast<const StructType*>(type.get())->getName();
			break;
		case TypeKind::Array: {
			auto* arrType = static_cast<const ArrayType*>(type.get());
			d.dim.push_back(arrType->getArrayLength());
			// Unroll multi-dimensional arrays if nested
			auto inner = arrType->getElementType();
			while (inner->getKind() == TypeKind::Array) {
				auto* innerArr = static_cast<const ArrayType*>(inner.get());
				d.dim.push_back(innerArr->getArrayLength());
				inner = innerArr->getElementType();
			}
			TypeDesc innerDesc = toDesc(inner);
			d.type = innerDesc.type;
			d.customName = innerDesc.customName;
			d.customTypeName = innerDesc.customTypeName;
			break;
		}
		case TypeKind::Function:
			d.type = DataType::Void;
			break;
		case TypeKind::Reference: {
			auto* refType = static_cast<const ReferenceType*>(type.get());
			d.isReference = true;
			d.isMutable = refType->isMutable();

			TypeDesc innerDesc = toDesc(refType->getInnerType());
			d.type = innerDesc.type;
			d.customName = innerDesc.customName;
			d.customTypeName = innerDesc.customTypeName;
			d.dim = innerDesc.dim;
			break;
		}
	}
	return d;
}

std::shared_ptr<Type> TypeChecker::typeOf(Expression& node) {
	currentType = prim(BuiltinType::Void); 
	node.accept(*this);
	return currentType ? currentType : prim(BuiltinType::Void);
}

std::shared_ptr<Type> TypeChecker::valueTypeOf(Expression& node) {
	return stripRef(typeOf(node));
}

bool TypeChecker::isWritable(Expression& e) {
	if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) {
		auto sym = scopeManager.lookup(id->name);
		if (!sym) return true; 
		auto ty = sym->getType();
		if (isRefType(ty)) return static_cast<const ReferenceType*>(ty.get())->isMutable();
		return true;
	}
	if (auto* ma = dynamic_cast<MemberAccessExpr*>(&e)) {
		if (!ma->object) return false;
		Expression& obj = *ma->object;
		if (dynamic_cast<IdentifierExpr*>(&obj) || dynamic_cast<MemberAccessExpr*>(&obj))
			return isWritable(obj);
		auto ty = typeOf(obj);
		return isRefType(ty) && static_cast<const ReferenceType*>(ty.get())->isMutable();
	}
	return false;
}

TypeDesc TypeChecker::check(Expression& node) {
	return toDesc(typeOf(node));
}

// ---------- Functions ----------

std::shared_ptr<FunctionType> TypeChecker::buildFunctionType(const FunctionDeclExpr& fn) {
	std::vector<std::shared_ptr<Type>> params;
	params.reserve(fn.params.size());
	for (const auto& p : fn.params) {
		params.push_back(fromDesc(p.type));
	}
	return std::make_shared<FunctionType>(fromDesc(fn.returnType), std::move(params));
}

std::shared_ptr<StructType> TypeChecker::buildStructureType(const StructDeclExpr& structures) {
	auto structure = std::make_shared<StructType>(structures.name);

	size_t currentOffset = 0;

	for (const auto& att : structures.attributes) {
		std::shared_ptr<Type> fieldType = fromDesc(att.type);

		size_t fieldOffset = currentOffset;

		structure->addField(att.name, fieldType, fieldOffset); 

		currentOffset += fieldType->getSize();
	}

	structure->setSize(currentOffset);

	return structure;
}

void TypeChecker::declareFunction(const FunctionDeclExpr& fn) {
	auto sym = std::make_shared<FunctionSymbol>(fn.name, buildFunctionType(fn));
	if (!scopeManager.declare(fn.name, sym)) {
		reportError(fn.line, fn.col, "Redeclaration of function: " + fn.name);
	}
	declaredFunctions_.insert(&fn);
}

void TypeChecker::declareStructures(const StructDeclExpr& structures) {
	auto sym = std::make_shared<StructSymbol>(structures.name, buildStructureType(structures));
	if (!scopeManager.declare(structures.name, sym)) {
		reportError(structures.line, structures.col, "Redeclaration of structure: " + structures.name);
	}
	declaredStructures_.insert(&structures);
}

void TypeChecker::initialize(const ExprPtr& m) {
	if (!m) return;

	if (auto* block = dynamic_cast<Block*>(m.get())) {
		for (const auto& expr : block->expressions) {
			if (auto* fn = dynamic_cast<FunctionDeclExpr*>(expr.get())) {
				this->declareFunction(*fn);
			}
			else if (auto* structObj = dynamic_cast<StructDeclExpr*>(expr.get()))
			{
				this->declareStructures(*structObj); 
			}
		}
	}

	typeOf(*m);
}

void TypeChecker::visit(FunctionDeclExpr& node) {
	if (!declaredFunctions_.count(&node)) declareFunction(node);

	scopeManager.enterScope();
	for (const auto& p : node.params) {
		auto sym = std::make_shared<VarSymbol>(p.name, fromDesc(p.type), /*isInitialized=*/true);
		localInfo_[sym.get()] = {};
		if (!scopeManager.declare(p.name, sym)) {
			reportError(node.line, node.col, "Duplicate parameter '" + p.name + "' in function " + node.name);
		}
	}

	auto prevReturn = currentReturnType;
	currentReturnType = fromDesc(node.returnType);
	if (node.body) typeOf(*node.body);
	currentReturnType = prevReturn;

	scopeManager.exitScope();
	currentType = prim(BuiltinType::Void);
}

void TypeChecker::visit(BorrowExpr& node) {
	if (!node.expr) {
		currentType = prim(BuiltinType::Void);
		return;
	}
	auto innerType = typeOf(*node.expr);

	if (node.isMutable && !isWritable(*node.expr)) {
		reportError(node.line, node.col, "Cannot take a mutable reference (&mut) here: the value is behind an immutable reference or is not an assignable location");
	}

	currentType = std::make_shared<ReferenceType>(stripRef(innerType), node.isMutable);
}

// Structure
void TypeChecker::visit(StructDeclExpr& node) {
	if (!declaredStructures_.count(&node)) declareStructures(node);

	scopeManager.enterScope();
	for (const auto& p : node.attributes) {
		auto sym = std::make_shared<VarSymbol>(p.name, fromDesc(p.type), /*isInitialized=*/true);
		if (!scopeManager.declare(p.name, sym)) {
			reportError(node.line, node.col, "Duplicate Attribute '" + p.name + "' in structure " + node.name);
		}
	}

	scopeManager.exitScope();
	currentType = prim(BuiltinType::Void);
}

void TypeChecker::visit(StructCreateExpr& node)
{
	if (!node.structObj) { currentType = prim(BuiltinType::Void); return; }

	auto structType = typeOf(*node.structObj);
	std::vector<std::shared_ptr<Type>> argTypes;
	for (auto& arg : node.arguments) {
		if (arg) argTypes.push_back(typeOf(*arg));
	}

	if (structType->getKind() != TypeKind::Struct) {
		reportError(node.line, node.col, "Called expression is not a structure");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* strct = static_cast<StructType*>(structType.get());
	const auto& fields = strct->getOrderedFields(); 

	if (fields.size() != argTypes.size()) {
		reportError(node.line, node.col, "Expected " + std::to_string(fields.size()) + " argument(s) but got " + std::to_string(argTypes.size()));
	} else {
		for (size_t i = 0; i < fields.size(); ++i) {
			if (!fields[i].type->equals(argTypes[i].get())) {
				reportError(node.line, node.col, "Argument " + std::to_string(i + 1) + " ('" + fields[i].name + "'): expected " + fields[i].type->toString() + " but got " + argTypes[i]->toString());
			}
		}
	}

	currentType = structType; 
}

void TypeChecker::visit(MemberAccessExpr &node)
{
	if (!node.object) { 
		currentType = prim(BuiltinType::Void); 
		return; 
	}

	auto baseType = typeOf(*node.object);

	if (baseType->getKind() == TypeKind::Reference) {
		baseType = static_cast<const ReferenceType*>(baseType.get())->getInnerType();
	}

	if (baseType->getKind() == TypeKind::Array || isPrim(baseType, BuiltinType::Str)) {
		if (node.memberName == "length") {
			currentType = prim(BuiltinType::Int);
			return;
		}
	}

	if (baseType->getKind() != TypeKind::Struct) {
		reportError(node.line, node.col, "Member access '.' applied to non-structure type");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* strct = static_cast<StructType*>(baseType.get());
	const auto* field = strct->getField(node.memberName);

	if (!field) {
		reportError(node.line, node.col, "Structure '" + strct->getName() + "' has no member named '" + node.memberName + "'");
		currentType = prim(BuiltinType::Void);
		return;
	}

	currentType = field->type;
}

bool isTypeCompatible(const std::shared_ptr<Type>& expected, const Type* actual) {
	if (!expected || !actual) return false;

	if (expected->getKind() == TypeKind::Reference && actual->getKind() == TypeKind::Reference) {
		auto* expRef = static_cast<const ReferenceType*>(expected.get());
		auto* actRef = static_cast<const ReferenceType*>(actual);

		if (!expRef->getInnerType()->equals(actRef->getInnerType().get())) return false;
		return !expRef->isMutable() || actRef->isMutable();
	}
	return expected->equals(actual);
}

void TypeChecker::visit(CallExpr& node) {
	if (!node.callee) { currentType = prim(BuiltinType::Void); return; }

	auto calleeType = typeOf(*node.callee);

	std::vector<std::shared_ptr<Type>> argTypes;
	for (auto& arg : node.arguments) {
		if (arg) argTypes.push_back(typeOf(*arg));
	}

	if (calleeType->getKind() != TypeKind::Function) {
		reportError(node.line, node.col, "Called expression is not a function");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* fn = static_cast<FunctionType*>(calleeType.get());
	const auto& params = fn->getParamTypes();

	if (params.size() != argTypes.size()) {
		reportError(node.line, node.col, "Expected " + std::to_string(params.size()) + " argument(s) but got " + std::to_string(argTypes.size()));
	} else {
		for (size_t i = 0; i < params.size(); ++i) {
			auto actual = isRefType(params[i]) ? argTypes[i] : stripRef(argTypes[i]);
			if (!isTypeCompatible(params[i], actual.get())) {
				reportError(node.line, node.col, "Argument " + std::to_string(i + 1) + ": expected " + params[i]->toString() + " but got " + argTypes[i]->toString());
			}
		}
	}
	currentType = fn->getReturnType();
}

void TypeChecker::visit(ReturnExpr& node) {
	std::shared_ptr<Type> valueType = prim(BuiltinType::Void);
	if (node.value && *node.value) {
		valueType = (currentReturnType && isRefType(currentReturnType))
			? typeOf(**node.value)
			: valueTypeOf(**node.value);
	}

	if (!currentReturnType) {
		reportError(node.line, node.col, "'return' outside of a function");
	} else if (!isTypeCompatible(currentReturnType, valueType.get())) {
		reportError(node.line, node.col, "Return type mismatch: expected " + currentReturnType->toString() + " but got " + valueType->toString());
	} else if (node.value && *node.value && carriesReference(valueType)) {
		std::string local = escapingLocal(**node.value);
		if (!local.empty())
			reportError(
					node.line, 
					node.col, 
					"cannot return a value that references local variable '" + local + "'",
					"local variable created here, but its scope ends when function returns."
					);
	}
	currentType = prim(BuiltinType::Void);
}

// ---------- Expressions ----------

void TypeChecker::visit(LiteralExpr& node) {
	switch (node.kind) {
		case LiteralKind::Int:   currentType = prim(BuiltinType::Int);   break;
		case LiteralKind::Float: currentType = prim(BuiltinType::Float); break;
		case LiteralKind::Bool:  currentType = prim(BuiltinType::Bool);  break;
		case LiteralKind::Char:  currentType = prim(BuiltinType::Char);  break;
		case LiteralKind::Str:   currentType = prim(BuiltinType::Str);   break;
	}
}

void TypeChecker::visit(ArrayLiteralExpr& node) {
	if (node.elements.empty()) {
		currentType = prim(BuiltinType::Void);
		return;
	}
	auto elementType = valueTypeOf(*node.elements[0]);
	for (size_t i = 1; i < node.elements.size(); ++i) {
		auto elemT = valueTypeOf(*node.elements[i]);
		if (!elementType->equals(elemT.get())) {
			reportError(node.line, node.col, "Mismatched types in array literal");
		}
	}
	currentType = std::make_shared<ArrayType>(elementType, node.elements.size());
}

void TypeChecker::visit(ArrayIndexExpr& node) {
	if (!node.array) {
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto currentExprType = typeOf(*node.array);
	if (isRefType(currentExprType)) {
		currentExprType = static_cast<const ReferenceType*>(currentExprType.get())->getInnerType();
	}

	// Iterate through each index in the vector (supports multi-dimensional indexing)
	for (size_t i = 0; i < node.indices.size(); ++i) {
		if (currentExprType->getKind() != TypeKind::Array) {
			reportError(node.line, node.col, "Indexing applied to non-array type: " + currentExprType->toString());
			currentType = prim(BuiltinType::Void);
			return;
		}

		auto* arr = static_cast<const ArrayType*>(currentExprType.get());

		if (node.indices[i]) {
			auto indexType = valueTypeOf(*node.indices[i]);
			if (!isPrim(indexType, BuiltinType::Int)) {
				reportError(node.line, node.col, "Array index must be an integer, got " + indexType->toString());
			}
		}

		// Advance to the inner type for the next dimension
		currentExprType = arr->getElementType();
	}

	currentType = currentExprType;
}

void TypeChecker::visit(IdentifierExpr& node) {
	auto symbol = scopeManager.lookup(node.name);
	if (!symbol) {
		reportError(node.line, node.col, "Undeclared identifier: " + node.name);
		currentType = prim(BuiltinType::Void);
		return;
	}
	currentType = symbol->getType();
}

void TypeChecker::visit(VarDeclExpr& node) {
	auto declared = fromDesc(node.type);

	if (node.expr) {
		bool keepRef = node.type.isReference || dynamic_cast<BorrowExpr*>(node.expr.get());
		auto initType = keepRef ? typeOf(*node.expr) : valueTypeOf(*node.expr);
		if (isPrim(declared, BuiltinType::Void)) {
			if (isRefType(initType))
				reportError(node.line, node.col, "Reference variable '" + tokenText(node.name) + "' needs an explicit type, e.g. &mut int " + tokenText(node.name));
			declared = initType; 
		} else if (!isTypeCompatible(declared, initType.get())) {
			reportError(node.line, node.col, "Type mismatch in declaration of " + tokenText(node.name) + ": expected " + declared->toString() + " but got " + initType->toString());
		}
	}

	auto sym = std::make_shared<VarSymbol>(tokenText(node.name), declared, node.expr != nullptr);
	if (!scopeManager.declare(tokenText(node.name), sym)) {
		reportError(node.line, node.col, "Redeclaration of variable: " + tokenText(node.name));
	}
	std::string dangling;
	if (node.expr && carriesReference(declared))
		dangling = escapingLocal(*node.expr);
	localInfo_[sym.get()] = { dangling };

	currentType = declared;
}

void TypeChecker::visit(BinaryExpr& node) {
	if (!node.left || !node.right) { currentType = prim(BuiltinType::Void); return; }

	auto l = valueTypeOf(*node.left);
	auto r = valueTypeOf(*node.right);

	switch (node.op) {
		case BinaryOp::Add:
			if (!l->equals(r.get())) {
				reportError(node.line, node.col, "Type mismatch in binary operation: " + l->toString() + " vs " + r->toString());
			} else if (!isNumeric(l) && !isPrim(l, BuiltinType::Str)) {
				reportError(node.line, node.col, "Operator '+' requires numeric or string operands, got " + l->toString());
			}
			currentType = l;
			break;

		case BinaryOp::Sub: case BinaryOp::Mul:
		case BinaryOp::Div: case BinaryOp::Mod: case BinaryOp::Power:
			if (!l->equals(r.get())) {
				reportError(node.line, node.col, "Type mismatch in binary operation: " + l->toString() + " vs " + r->toString());
			} else if (!isNumeric(l)) {
				reportError(node.line, node.col, "Arithmetic operator requires numeric operands, got " + l->toString());
			}
			currentType = l;
			break;

		case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
		case BinaryOp::Gt: case BinaryOp::Le: case BinaryOp::Ge:
			if (!l->equals(r.get())) {
				reportError(node.line, node.col, "Cannot compare " + l->toString() + " with " + r->toString());
			}
			currentType = prim(BuiltinType::Bool);
			break;

		case BinaryOp::And: case BinaryOp::Or:
			if (!isPrim(l, BuiltinType::Bool) || !isPrim(r, BuiltinType::Bool)) {
				reportError(node.line, node.col, "Logical operator requires bool operands");
			}
			currentType = prim(BuiltinType::Bool);
			break;

		case BinaryOp::BitAnd:
			if (!isPrim(l, BuiltinType::Int) || !isPrim(r, BuiltinType::Int)) {
				reportError(node.line, node.col, "Bitwise operator requires int operands");
			}
			currentType = prim(BuiltinType::Int);
			break;
	}
}

void TypeChecker::visit(UnaryExpr& node) {
	if (!node.expr) { currentType = prim(BuiltinType::Void); return; }

	auto t = valueTypeOf(*node.expr);
	switch (node.op) {
		case UnaryOp::Negate:
			if (!isNumeric(t)) reportError(node.line, node.col, "Cannot negate " + t->toString());
			currentType = t;
			break;
		case UnaryOp::Not:
			if (!isPrim(t, BuiltinType::Bool)) reportError(node.line, node.col, "'!' requires a bool, got " + t->toString());
			currentType = prim(BuiltinType::Bool);
			break;
	}
}

void TypeChecker::visit(Block& node) {
	scopeManager.enterScope();
	std::shared_ptr<Type> last = prim(BuiltinType::Void);
	for (auto& expr : node.expressions) {
		if (expr) last = typeOf(*expr);
	}
	scopeManager.exitScope();
	currentType = last; 
}

void TypeChecker::visit(IfExpr& node) {
	if (node.condition) {
		auto c = valueTypeOf(*node.condition);
		if (!isPrim(c, BuiltinType::Bool)) {
			reportError(node.line, node.col, "If condition must be bool, got " + c->toString());
		}
	}

	std::shared_ptr<Type> thenType = node.ifBranch ? typeOf(*node.ifBranch) : prim(BuiltinType::Void);

	if (node.elseBranch && *node.elseBranch) {
		auto elseType = typeOf(**node.elseBranch);
		currentType = thenType->equals(elseType.get()) ? thenType : prim(BuiltinType::Void);
	} else {
		currentType = prim(BuiltinType::Void);
	}
}

void TypeChecker::visit(WhileExpr& node) {
	if (node.condition) {
		auto c = valueTypeOf(*node.condition);
		if (!isPrim(c, BuiltinType::Bool)) {
			reportError(node.line, node.col, "While condition must be bool, got " + c->toString());
		}
	}
	if (node.loopBranch) typeOf(*node.loopBranch);
	currentType = prim(BuiltinType::Void);
}

void TypeChecker::visit(LoopExpr& node) {
	if (node.loopBranch) typeOf(*node.loopBranch);
	currentType = prim(BuiltinType::Void);
}

void TypeChecker::visit(ImportExpr& node) {
	if (!pipeline) return;

	std::string actualPath = "";
	for (const auto& seg : node.path.segments) {
		actualPath.append(seg + "/");
	}
	if (!actualPath.empty()) actualPath.erase(actualPath.size() - 1);
	actualPath.append(".op");

	fs::path targetPath(actualPath);
	if (!currentFilePath.empty() && targetPath.is_relative()) {
		fs::path parentDir = fs::path(currentFilePath).parent_path();
		targetPath = (parentDir / targetPath).lexically_normal();
	}

	const Module* importedModule = this->pipeline->getModuleByPath(targetPath.string());
	if (!importedModule || !importedModule->astRoot) {
		reportError(node.line, node.col, "Could not resolve imported module: " + targetPath.string());
		currentType = prim(BuiltinType::Void);
		return;
	}
	if (auto* block = dynamic_cast<Block*>(importedModule->astRoot.get())) {
		for (const auto& expr : block->expressions) {
			if (auto* fn = dynamic_cast<FunctionDeclExpr*>(expr.get())) {
				if (fn->isPublic) {
					if (node.isSelectiveImport && !(std::find(node.importedSymbols.begin(), node.importedSymbols.end(), fn->name) != node.importedSymbols.end())) continue;

					auto fnType = buildFunctionType(*fn);
					auto sym = std::make_shared<FunctionSymbol>(fn->name, fnType);
					scopeManager.declare(fn->name, sym);
				}
			}
			else if (auto* structure = dynamic_cast<StructDeclExpr*>(expr.get())) {
				if (structure->isPublic) {
					if (node.isSelectiveImport && !(std::find(node.importedSymbols.begin(), node.importedSymbols.end(), structure->name) != node.importedSymbols.end())) continue;

					auto structType = buildStructureType(*structure);
					auto sym = std::make_shared<StructSymbol>(structure->name, structType);
					scopeManager.declare(structure->name, sym);
				}
			}
		}
	}

	reportError(node.line, node.col, "The imported module " + actualPath + " doesn't exist");
	currentType = prim(BuiltinType::Void);
}

void TypeChecker::visit(CastExpr& node) {
	if (!node.expr) { currentType = prim(BuiltinType::Void); return; }

	auto t = fromDesc(node.castType);
	currentType = t;
}

// ---------- Assignment / ++ -- (with mutability enforcement) ----------

void TypeChecker::visit(AssignExpr& node) {
	if (!node.target || !node.value) { currentType = prim(BuiltinType::Void); return; }

	auto targetType = valueTypeOf(*node.target);
	auto valueType  = valueTypeOf(*node.value);

	if (!isWritable(*node.target)) {
		reportError(node.line, node.col, "Cannot assign through an immutable reference (declare it as &mut)");
	}
	if (!targetType->equals(valueType.get())) {
		reportError(node.line, node.col, "Cannot assign " + valueType->toString() + " to " + targetType->toString());
	}
	currentType = targetType;
}

void TypeChecker::visit(PostfixExpr& node) {
	if (!node.expr) { currentType = prim(BuiltinType::Void); return; }

	auto t = valueTypeOf(*node.expr);
	if (!isNumeric(t)) reportError(node.line, node.col, "++/-- requires a numeric operand, got " + t->toString());
	if (!isWritable(*node.expr)) {
		reportError(node.line, node.col, "Cannot modify a value through an immutable reference (declare it as &mut)");
	}
	currentType = t;
}
