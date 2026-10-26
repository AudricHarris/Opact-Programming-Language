#include "TypeChecker.hpp"
#include "../Parser/Ast.hpp"
#include "../Parser/CompilerPipeline.hpp"
#include "Symbol/FunctionSymbol.hpp"
#include "Symbol/StructSymbol.hpp"
#include "Symbol/VarSymbol.hpp"
#include "Types/PrimitiveType.hpp"
#include "Types/ReferenceType.hpp"
#include "Types/StructType.hpp"
#include "Types/Type.hpp"
#include <algorithm>
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

void TypeChecker::reportError(const std::string& message) {
	++errorCount_;
	this->correctCode = false;
	std::cerr << "Type error: " << message << "\n";
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
		case TypeKind::Function:
			// TypeDesc cannot describe function types; Void is the fallback.
			d.type = DataType::Void;
			break;
		case TypeKind::Reference:
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
	return d;
}

std::shared_ptr<Type> TypeChecker::typeOf(Expression& node) {
	currentType = prim(BuiltinType::Void); // never leak a stale result
	node.accept(*this);
	return currentType ? currentType : prim(BuiltinType::Void);
}

// Type of `node` when used as a value: references are dereferenced.
std::shared_ptr<Type> TypeChecker::valueTypeOf(Expression& node) {
	return stripRef(typeOf(node));
}

// Can the location denoted by `e` be written to?
//   x          -> plain variable: yes;  reference variable: only if `&mut`
//   x.a.b      -> writable iff the root `x` is (a `&` reference makes it read-only)
//   anything else (literals, arithmetic, ...) is not an assignable location,
//   except a call returning `&mut T`, which is handled via its type.
bool TypeChecker::isWritable(Expression& e) {
	if (auto* id = dynamic_cast<IdentifierExpr*>(&e)) {
		auto sym = scopeManager.lookup(id->name);
		if (!sym) return true; // "undeclared identifier" is reported elsewhere
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

		structure->addField(att.name, fieldType, fieldOffset); // <-- Register the field here!

		currentOffset += fieldType->getSize();
	}

	structure->setSize(currentOffset);

	return structure;
}

void TypeChecker::declareFunction(const FunctionDeclExpr& fn) {
	auto sym = std::make_shared<FunctionSymbol>(fn.name, buildFunctionType(fn));
	if (!scopeManager.declare(fn.name, sym)) {
		reportError("Redeclaration of function: " + fn.name);
	}
	declaredFunctions_.insert(&fn);
}

void TypeChecker::declareStructures(const StructDeclExpr& structures) {
	auto sym = std::make_shared<StructSymbol>(structures.name, buildStructureType(structures));
	if (!scopeManager.declare(structures.name, sym)) {
		reportError("Redeclaration of structure: " + structures.name);
	}
	declaredStructures_.insert(&structures);
}

void TypeChecker::initialize(const ExprPtr& m) {
	if (!m) return;

	// Hoist top-level functions into the global scope
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
	// Already hoisted by initialize()? Otherwise (nested function) declare now.
	if (!declaredFunctions_.count(&node)) declareFunction(node);

	scopeManager.enterScope();
	for (const auto& p : node.params) {
		auto sym = std::make_shared<VarSymbol>(p.name, fromDesc(p.type), /*isInitialized=*/true);
		if (!scopeManager.declare(p.name, sym)) {
			reportError("Duplicate parameter '" + p.name + "' in function " + node.name);
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
		reportError("Cannot take a mutable reference (&mut) here: the value is behind an "
				"immutable reference or is not an assignable location");
	}

	// Re-borrowing `&r` of a reference variable still points at the original object.
	currentType = std::make_shared<ReferenceType>(stripRef(innerType), node.isMutable);
}

// Structure
void TypeChecker::visit(StructDeclExpr& node) {
	if (!declaredStructures_.count(&node)) declareStructures(node);

	scopeManager.enterScope();
	for (const auto& p : node.attributes) {
		auto sym = std::make_shared<VarSymbol>(p.name, fromDesc(p.type), /*isInitialized=*/true);
		if (!scopeManager.declare(p.name, sym)) {
			reportError("Duplicate Attribute '" + p.name + "' in structure " + node.name);
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
		reportError("Called expression is not a structure");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* strct = static_cast<StructType*>(structType.get());
	const auto& fields = strct->getOrderedFields(); // Uses declaration order

	if (fields.size() != argTypes.size()) {
		reportError("Expected " + std::to_string(fields.size()) + " argument(s) but got " +
				std::to_string(argTypes.size()));
	} else {
		for (size_t i = 0; i < fields.size(); ++i) {
			if (!fields[i].type->equals(argTypes[i].get())) {
				reportError("Argument " + std::to_string(i + 1) + " ('" + fields[i].name + "'): expected " +
						fields[i].type->toString() + " but got " + argTypes[i]->toString());
			}
		}
	}

	currentType = structType; // Evaluates to the instance type of the structure
}

void TypeChecker::visit(MemberAccessExpr &node)
{
	if (!node.object) { 
		currentType = prim(BuiltinType::Void); 
		return; 
	}

	auto baseType = typeOf(*node.object);

	// Auto-dereference if the object is a reference to a struct
	if (baseType->getKind() == TypeKind::Reference) {
		baseType = static_cast<const ReferenceType*>(baseType.get())->getInnerType();
	}

	if (baseType->getKind() != TypeKind::Struct) {
		reportError("Member access '.' applied to non-structure type");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* strct = static_cast<StructType*>(baseType.get());
	const auto* field = strct->getField(node.memberName);

	if (!field) {
		reportError("Structure '" + strct->getName() + "' has no member named '" + node.memberName + "'");
		currentType = prim(BuiltinType::Void);
		return;
	}

	currentType = field->type;
}


bool isTypeCompatible(const std::shared_ptr<Type>& expected, const Type* actual) {
	if (!expected || !actual) return false;

	// Checked BEFORE equals(): we cannot rely on Type::equals comparing mutability.
	if (expected->getKind() == TypeKind::Reference && actual->getKind() == TypeKind::Reference) {
		auto* expRef = static_cast<const ReferenceType*>(expected.get());
		auto* actRef = static_cast<const ReferenceType*>(actual);

		if (!expRef->getInnerType()->equals(actRef->getInnerType().get())) return false;
		// &mut T -> &T is fine, &T -> &mut T is not
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
		reportError("Called expression is not a function");
		currentType = prim(BuiltinType::Void);
		return;
	}

	auto* fn = static_cast<FunctionType*>(calleeType.get());
	const auto& params = fn->getParamTypes();

	if (params.size() != argTypes.size()) {
		reportError("Expected " + std::to_string(params.size()) + " argument(s) but got " +
				std::to_string(argTypes.size()));
	} else {
		for (size_t i = 0; i < params.size(); ++i) {
			// Use isTypeCompatible instead of strict equals
			auto actual = isRefType(params[i]) ? argTypes[i] : stripRef(argTypes[i]);
			if (!isTypeCompatible(params[i], actual.get())) {
				reportError("Argument " + std::to_string(i + 1) + ": expected " +
						params[i]->toString() + " but got " + argTypes[i]->toString());
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
		reportError("'return' outside of a function");
	} else if (!isTypeCompatible(currentReturnType, valueType.get())) {
		reportError("Return type mismatch: expected " + currentReturnType->toString() +
				" but got " + valueType->toString());
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

void TypeChecker::visit(IdentifierExpr& node) {
	auto symbol = scopeManager.lookup(node.name);
	if (!symbol) {
		reportError("Undeclared identifier: " + node.name);
		currentType = prim(BuiltinType::Void);
		return;
	}
	currentType = symbol->getType();
}

void TypeChecker::visit(VarDeclExpr& node) {
	auto declared = fromDesc(node.type);

	if (node.expr) {
		// A reference is kept as a reference only if the variable is declared as
		// one (or the initializer is an explicit borrow); otherwise it is read as a value.
		bool keepRef = node.type.isReference || dynamic_cast<BorrowExpr*>(node.expr.get());
		auto initType = keepRef ? typeOf(*node.expr) : valueTypeOf(*node.expr);
		if (isPrim(declared, BuiltinType::Void)) {
			if (isRefType(initType))
				reportError("Reference variable '" + tokenText(node.name) +
						"' needs an explicit type, e.g. &mut int " + tokenText(node.name));
			declared = initType; // no explicit type given: infer from initializer
		} else if (!isTypeCompatible(declared, initType.get())) {
			reportError("Type mismatch in declaration of " + tokenText(node.name) +
					": expected " + declared->toString() + " but got " + initType->toString());
		}
	}

	auto sym = std::make_shared<VarSymbol>(tokenText(node.name), declared, node.expr != nullptr);
	if (!scopeManager.declare(tokenText(node.name), sym)) {
		reportError("Redeclaration of variable: " + tokenText(node.name));
	}
	currentType = declared;
}

void TypeChecker::visit(BinaryExpr& node) {
	if (!node.left || !node.right) { currentType = prim(BuiltinType::Void); return; }

	auto l = valueTypeOf(*node.left);
	auto r = valueTypeOf(*node.right);

	switch (node.op) {
		case BinaryOp::Add: case BinaryOp::Sub: case BinaryOp::Mul:
		case BinaryOp::Div: case BinaryOp::Mod: case BinaryOp::Power:
			if (!l->equals(r.get())) {
				reportError("Type mismatch in binary operation: " + l->toString() + " vs " + r->toString());
			} else if (!isNumeric(l)) {
				reportError("Arithmetic operator requires numeric operands, got " + l->toString());
			}
			currentType = l;
			break;

		case BinaryOp::Eq: case BinaryOp::Ne: case BinaryOp::Lt:
		case BinaryOp::Gt: case BinaryOp::Le: case BinaryOp::Ge:
			if (!l->equals(r.get())) {
				reportError("Cannot compare " + l->toString() + " with " + r->toString());
			}
			currentType = prim(BuiltinType::Bool);
			break;

		case BinaryOp::And: case BinaryOp::Or:
			if (!isPrim(l, BuiltinType::Bool) || !isPrim(r, BuiltinType::Bool)) {
				reportError("Logical operator requires bool operands");
			}
			currentType = prim(BuiltinType::Bool);
			break;

		case BinaryOp::BitAnd:
			if (!isPrim(l, BuiltinType::Int) || !isPrim(r, BuiltinType::Int)) {
				reportError("Bitwise operator requires int operands");
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
			if (!isNumeric(t)) reportError("Cannot negate " + t->toString());
			currentType = t;
			break;
		case UnaryOp::Not:
			if (!isPrim(t, BuiltinType::Bool)) reportError("'!' requires a bool, got " + t->toString());
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
	//scopeManager.dump();
	scopeManager.exitScope();
	currentType = last; // a block evaluates to its last expression
}

void TypeChecker::visit(IfExpr& node) {
	if (node.condition) {
		auto c = valueTypeOf(*node.condition);
		if (!isPrim(c, BuiltinType::Bool)) {
			reportError("If condition must be bool, got " + c->toString());
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
			reportError("While condition must be bool, got " + c->toString());
		}
	}
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
		reportError("Could not resolve imported module: " + targetPath.string());
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
	scopeManager.dump();
	currentType = prim(BuiltinType::Void);
}


// ---------- Assignment / ++ -- (with mutability enforcement) ----------

void TypeChecker::visit(AssignExpr& node) {
	if (!node.target || !node.value) { currentType = prim(BuiltinType::Void); return; }

	// For `r = v` / `r.x = v` the left side is the *referenced* value.
	auto targetType = valueTypeOf(*node.target);
	auto valueType  = valueTypeOf(*node.value);

	if (!isWritable(*node.target)) {
		reportError("Cannot assign through an immutable reference (declare it as &mut)");
	}
	if (!targetType->equals(valueType.get())) {
		reportError("Cannot assign " + valueType->toString() + " to " + targetType->toString());
	}
	currentType = targetType;
}

void TypeChecker::visit(PostfixExpr& node) {
	if (!node.expr) { currentType = prim(BuiltinType::Void); return; }

	auto t = valueTypeOf(*node.expr);
	if (!isNumeric(t)) reportError("++/-- requires a numeric operand, got " + t->toString());
	if (!isWritable(*node.expr)) {
		reportError("Cannot modify a value through an immutable reference (declare it as &mut)");
	}
	currentType = t;
}
