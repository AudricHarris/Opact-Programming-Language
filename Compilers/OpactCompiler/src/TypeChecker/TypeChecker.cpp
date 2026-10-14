#include "TypeChecker.hpp"
#include "../Parser/Ast.hpp"
#include "Parser/CompilerPipeline.hpp"
#include "TypeChecker/Symbol/StructSymbol.hpp"
#include "TypeChecker/Symbol/VarSymbol.hpp"
#include "TypeChecker/Types/StructType.hpp"
#include <algorithm>
#include <iostream>
#include <utility>

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
} // namespace



TypeChecker::TypeChecker(CompilerPipeline* pipeline, const std::string& filePath)
	: pipeline(pipeline), currentFilePath(filePath), currentType(prim(BuiltinType::Void)) {}

void TypeChecker::reportError(const std::string& message) {
	++errorCount_;
	std::cerr << "Type error: " << message << "\n";
}

// ---------- Type conversion helpers ----------

std::shared_ptr<Type> TypeChecker::fromDesc(const TypeDesc& d) {
	switch (d.type) {
		case DataType::Int:   return prim(BuiltinType::Int);
		case DataType::Float: return prim(BuiltinType::Float);
		case DataType::Bool:  return prim(BuiltinType::Bool);
		case DataType::Char:  return prim(BuiltinType::Char);
		case DataType::Str:   return prim(BuiltinType::Str);
		case DataType::Void:  return prim(BuiltinType::Void);
		case DataType::Custom: {
			std::string name = d.customTypeName;
			if (name.empty() && d.customName) name = tokenText(*d.customName);
			auto sym = scopeManager.lookup(name);
			if (!sym || sym->getKind() != SymbolKind::Class) {
				reportError("Unknown type: " + name);
				return prim(BuiltinType::Void);
			}
			return sym->getType();
		}
	}
	return prim(BuiltinType::Void);
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
	}
	return d;
}

std::shared_ptr<Type> TypeChecker::typeOf(Expression& node) {
	currentType = prim(BuiltinType::Void); // never leak a stale result
	node.accept(*this);
	return currentType ? currentType : prim(BuiltinType::Void);
}

TypeDesc TypeChecker::check(Expression& node) {
	return toDesc(typeOf(node));
}

// ---------- Functions ----------

std::shared_ptr<FunctionType> TypeChecker::buildFunctionType(const FunctionDeclExpr& fn) {
	std::vector<std::shared_ptr<Type>> params;
	params.reserve(fn.params.size());
	for (const auto& p : fn.params) params.push_back(fromDesc(p.type));
	return std::make_shared<FunctionType>(fromDesc(fn.returnType), std::move(params));
}

std::shared_ptr<StructType> TypeChecker::buildStructureType(const StructDeclExpr& structures) {
	auto structure = std::make_shared<StructType>(structures.name);

	for ( const auto& att : structures.attributes)
	{
		std::cout << att.name << "\n";
	}

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
			if (!params[i]->equals(argTypes[i].get())) {
				reportError("Argument " + std::to_string(i + 1) + ": expected " +
				            params[i]->toString() + " but got " + argTypes[i]->toString());
			}
		}
	}
	currentType = fn->getReturnType();
}

void TypeChecker::visit(ReturnExpr& node) {
	std::shared_ptr<Type> valueType = (node.value && *node.value)
		? typeOf(**node.value)
		: prim(BuiltinType::Void);

	if (!currentReturnType) {
		reportError("'return' outside of a function");
	} else if (!currentReturnType->equals(valueType.get())) {
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
		auto initType = typeOf(*node.expr);
		if (isPrim(declared, BuiltinType::Void)) {
			declared = initType; // no explicit type given: infer from initializer
		} else if (!declared->equals(initType.get())) {
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

	auto l = typeOf(*node.left);
	auto r = typeOf(*node.right);

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

	auto t = typeOf(*node.expr);
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
	scopeManager.dump();
	scopeManager.exitScope();
	currentType = last; // a block evaluates to its last expression
}

void TypeChecker::visit(IfExpr& node) {
	if (node.condition) {
		auto c = typeOf(*node.condition);
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
		auto c = typeOf(*node.condition);
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
		}
	}

	currentType = prim(BuiltinType::Void);
}
