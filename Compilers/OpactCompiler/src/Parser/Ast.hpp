#ifndef AST_HPP
#define AST_HPP

#include "../Token/TokenType.hpp"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Forward declarations for AST Visitor Pattern
struct ASTVisitor;

//----------------------//
//-- Type System Spec --//
//----------------------//

enum class DataType { Int, Float, Bool, Char, Str, Void, Custom };

struct TypeDesc {
	DataType type = DataType::Void;
	std::optional<Token> customName;
	std::string customTypeName;
	std::vector<int> dim;

	bool isReference = false;
	bool isMutable	 = false;
	bool isDynamic	 = false;

	[[nodiscard]] bool isArray() const { return !dim.empty(); }
};

//--------------------------//
//-- Base Node Definition --//
//--------------------------//

struct Expression {
	virtual ~Expression() = default;
	virtual void accept(ASTVisitor& visitor) = 0;
	[[nodiscard]] virtual std::unique_ptr<Expression> clone() const = 0;
	[[nodiscard]] virtual bool requiresSemicolon() const { return true; }
};

using ExprPtr = std::unique_ptr<Expression>;

// Helper function to safely clone optional ExprPtr
inline std::optional<ExprPtr> cloneOptional(const std::optional<ExprPtr>& expr) {
	if (!expr.has_value() || !*expr) return std::nullopt;
	return (*expr)->clone();
}

// Helper function to clone vectors of ExprPtr
inline std::vector<ExprPtr> cloneVector(const std::vector<ExprPtr>& vec) {
	std::vector<ExprPtr> copy;
	copy.reserve(vec.size());
	for (const auto& item : vec) {
		if (item) copy.push_back(item->clone());
	}
	return copy;
}

//----------------------------//
//-- Literals & Identifiers --//
//----------------------------//

enum class LiteralKind { Int, Float, Bool, Char, Str };
enum class NumericBase { Decimal, Hexadecimal, Octal, Binary };

struct LiteralExpr : Expression {
	LiteralKind kind;
	Token lit;
	NumericBase base = NumericBase::Decimal;

	LiteralExpr(LiteralKind kind, Token lit, NumericBase base = NumericBase::Decimal)
		: kind(kind), lit(std::move(lit)), base(base) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<LiteralExpr>(kind, lit, base);
	}
};

struct IdentifierExpr : Expression {
	std::string name;

	explicit IdentifierExpr(std::string name)
		: name(std::move(name)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<IdentifierExpr>(name);
	}
};


//------------------------------------------//
//-- Operators & Unary/Binary Expressions --//
//------------------------------------------//

enum class BinaryOp {
	Add, Sub, Mul, Div, Mod,
	Eq, Ne, Lt, Gt, Le, Ge,
	And, BitAnd, Or, Power
};

enum class UnaryOp { Negate, Not };
enum class PostfixOp { Increment, Decrement };

struct BinaryExpr : Expression {
	BinaryOp op;
	ExprPtr left;
	ExprPtr right;

	BinaryExpr(BinaryOp op, ExprPtr left, ExprPtr right)
		: op(op), left(std::move(left)), right(std::move(right)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<BinaryExpr>(op, left ? left->clone() : nullptr, right ? right->clone() : nullptr);
	}
};

struct UnaryExpr : Expression {
	UnaryOp op;
	ExprPtr expr;

	UnaryExpr(UnaryOp op, ExprPtr expr)
		: op(op), expr(std::move(expr)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<UnaryExpr>(op, expr ? expr->clone() : nullptr);
	}
};

struct PostfixExpr : Expression {
	PostfixOp op;
	ExprPtr expr;

	PostfixExpr(PostfixOp op, ExprPtr expr)
		: op(op), expr(std::move(expr)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<PostfixExpr>(op, expr ? expr->clone() : nullptr);
	}
};

struct BorrowExpr : Expression {
	ExprPtr expr;
	bool isMutable;

	explicit BorrowExpr(ExprPtr expr, bool isMutable = false)
		: expr(std::move(expr)), isMutable(isMutable) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<BorrowExpr>(expr ? expr->clone() : nullptr, isMutable);
	}
};

//----------------------------//
//-- Function Calls & Casts --//
//----------------------------//

struct CallExpr : Expression {
	ExprPtr callee;
	std::vector<ExprPtr> arguments;

	CallExpr(ExprPtr callee = nullptr, std::vector<ExprPtr> arguments = {})
		: callee(std::move(callee)), arguments(std::move(arguments)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<CallExpr>(callee ? callee->clone() : nullptr, cloneVector(arguments));
	}
};

struct GenericCallExpr : Expression {
	ExprPtr callee;
	std::vector<TypeDesc> typeArgs;
	std::vector<ExprPtr> arguments;

	GenericCallExpr(ExprPtr callee, std::vector<TypeDesc> typeArgs, std::vector<ExprPtr> arguments)
		: callee(std::move(callee)), typeArgs(std::move(typeArgs)), arguments(std::move(arguments)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<GenericCallExpr>(callee ? callee->clone() : nullptr, typeArgs, cloneVector(arguments));
	}
};

struct CastExpr : Expression {
	ExprPtr expr;
	TypeDesc castType;

	CastExpr(ExprPtr expr, TypeDesc castType)
		: expr(std::move(expr)), castType(std::move(castType)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<CastExpr>(expr ? expr->clone() : nullptr, castType);
	}
};

//-------------------------------//
//-- Control Flow & Statements --//
//-------------------------------//

enum class AssignKind { Assign, Move, Borrow };

struct AssignExpr : Expression {
	ExprPtr target;
	ExprPtr value;
	AssignKind kind;

	AssignExpr(ExprPtr target, ExprPtr value, AssignKind kind = AssignKind::Assign)
		: target(std::move(target)), value(std::move(value)), kind(kind) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<AssignExpr>(target ? target->clone() : nullptr, value ? value->clone() : nullptr, kind);
	}
};

struct VarDeclExpr : Expression {
	Token name;
	TypeDesc type;
	ExprPtr expr;

	VarDeclExpr(Token name, TypeDesc type, ExprPtr expr = nullptr)
		: name(std::move(name)), type(std::move(type)), expr(std::move(expr)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<VarDeclExpr>(name, type, expr ? expr->clone() : nullptr);
	}
};

struct Block : Expression {
	std::vector<ExprPtr> expressions;

	explicit Block(std::vector<ExprPtr> expressions = {})
		: expressions(std::move(expressions)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<Block>(cloneVector(expressions));
	}
};

struct IfExpr : Expression {
	ExprPtr condition;
	ExprPtr ifBranch;
	std::optional<ExprPtr> elseBranch;

	IfExpr(ExprPtr condition = nullptr, ExprPtr ifBranch = nullptr, std::optional<ExprPtr> elseBranch = std::nullopt)
		: condition(std::move(condition)), ifBranch(std::move(ifBranch)), elseBranch(std::move(elseBranch)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<IfExpr>(
			condition ? condition->clone() : nullptr,
			ifBranch ? ifBranch->clone() : nullptr,
			cloneOptional(elseBranch)
		);
	}
	
	[[nodiscard]] virtual bool requiresSemicolon() const override { return false; }
};

struct WhileExpr : Expression {
	ExprPtr condition;
	ExprPtr loopBranch;

	WhileExpr(ExprPtr condition = nullptr, ExprPtr loopBranch = nullptr)
		: condition(std::move(condition)), loopBranch(std::move(loopBranch)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<WhileExpr>(
			condition ? condition->clone() : nullptr,
			loopBranch ? loopBranch->clone() : nullptr
		);
	}
	
	[[nodiscard]] virtual bool requiresSemicolon() const override { return false; }
};

struct LoopExpr : Expression {
	ExprPtr loopBranch;

	LoopExpr(ExprPtr loopBranch = nullptr)
		: loopBranch(std::move(loopBranch)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<LoopExpr>(
			loopBranch ? loopBranch->clone() : nullptr
		);
	}
	
	[[nodiscard]] virtual bool requiresSemicolon() const override { return false; }
};

struct MemberAccessExpr : Expression {
	ExprPtr object;
	std::string memberName;

	MemberAccessExpr(ExprPtr object, std::string memberName)
		: object(std::move(object)), memberName(std::move(memberName)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<MemberAccessExpr>(object ? object->clone() : nullptr, memberName);
	}
};

struct ArrayIndexExpr : Expression {
	ExprPtr array;
	std::vector<ExprPtr> indices;

	ArrayIndexExpr(ExprPtr array, std::vector<ExprPtr> indices)
		: array(std::move(array)), indices(std::move(indices)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<ArrayIndexExpr>(array ? array->clone() : nullptr, cloneVector(indices));
	}
};

struct ArrayLiteralExpr : Expression {
	std::vector<ExprPtr> elements;

	explicit ArrayLiteralExpr(std::vector<ExprPtr> elements)
		: elements(std::move(elements)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<ArrayLiteralExpr>(cloneVector(elements));
	}
};

struct TupleLiteralExpr : Expression {
	std::vector<ExprPtr> elements;

	explicit TupleLiteralExpr(std::vector<ExprPtr> elements)
		: elements(std::move(elements)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<TupleLiteralExpr>(cloneVector(elements));
	}
};

struct YieldExpr : Expression {
	ExprPtr expr;

	explicit YieldExpr(ExprPtr expr)
		: expr(std::move(expr)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<YieldExpr>(expr ? expr->clone() : nullptr);
	}
};

struct BreakExpr : Expression {
	std::optional<ExprPtr> value;

	explicit BreakExpr(std::optional<ExprPtr> value = std::nullopt)
		: value(std::move(value)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<BreakExpr>(cloneOptional(value));
	}
};

struct ReturnExpr : Expression {
	std::optional<ExprPtr> value;

	explicit ReturnExpr(std::optional<ExprPtr> value = std::nullopt)
		: value(std::move(value)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<ReturnExpr>(cloneOptional(value));
	}
};

struct ContinueExpr : Expression {
	ContinueExpr() = default;

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<ContinueExpr>();
	}
};

//-----------------------//
//-- Modules & Imports --//
//-----------------------//

struct ImportPath {
	std::vector<std::string> segments;
	bool isStdLib = false;
};

struct ImportExpr : Expression {
	ImportPath path;
	std::vector<std::string> importedSymbols;
	bool isSelectiveImport = false;

	ImportExpr(ImportPath path = {}, std::vector<std::string> importedSymbols = {}, bool isSelectiveImport = false)
		: path(std::move(path)), importedSymbols(std::move(importedSymbols)), isSelectiveImport(isSelectiveImport) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<ImportExpr>(path, importedSymbols, isSelectiveImport);
	}
};

//----------------------------//
//-- Functions & Parameters --//
//----------------------------//

struct Parameter {
	std::string name;
	TypeDesc type;
	bool isMutable = false;
	bool isReference = false;
	std::optional<ExprPtr> defaultVal = std::nullopt;

	Parameter(std::string name, TypeDesc type, bool isMut = false, bool isRef = false, std::optional<ExprPtr> defaultVal = std::nullopt)
		: name(std::move(name)), type(std::move(type)), isMutable(isMut), isReference(isRef), defaultVal(std::move(defaultVal)) {}

	// Move-only due to unique_ptr in defaultVal
	Parameter(const Parameter&) = delete;
	Parameter& operator=(const Parameter&) = delete;
	Parameter(Parameter&&) noexcept = default;
	Parameter& operator=(Parameter&&) noexcept = default;

	[[nodiscard]] Parameter clone() const {
		return Parameter(name, type, isMutable, isReference, cloneOptional(defaultVal));
	}
};

struct FunctionDeclExpr : Expression {
	std::string name;
	std::vector<Parameter> params;
	TypeDesc returnType;
	ExprPtr body;
	bool isPublic = false;
	bool isStatic = false;

	FunctionDeclExpr(std::string name = "", std::vector<Parameter> params = {}, TypeDesc returnType = {}, ExprPtr body = nullptr, bool isPublic = false, bool isStatic = false)
		: name(std::move(name)), params(std::move(params)), returnType(std::move(returnType)), body(std::move(body)), isPublic(isPublic), isStatic(isStatic) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		std::vector<Parameter> clonedParams;
		clonedParams.reserve(params.size());
		for (const auto& p : params) {
			clonedParams.push_back(p.clone());
		}
		return std::make_unique<FunctionDeclExpr>(name, std::move(clonedParams), returnType, body ? body->clone() : nullptr, isPublic, isStatic);
	}
	
	[[nodiscard]] virtual bool requiresSemicolon() const override { return false; }
};

//-----------------------//
//-- Visitor Interface --//
//-----------------------//

struct ASTVisitor {
	virtual ~ASTVisitor() = default;
	virtual void visit(LiteralExpr& node) = 0;
	virtual void visit(IdentifierExpr& node) = 0;
	virtual void visit(BinaryExpr& node) = 0;
	virtual void visit(UnaryExpr& node) = 0;
	virtual void visit(PostfixExpr& node) = 0;
	virtual void visit(BorrowExpr& node) = 0;
	virtual void visit(CallExpr& node) = 0;
	virtual void visit(GenericCallExpr& node) = 0;
	virtual void visit(CastExpr& node) = 0;
	virtual void visit(AssignExpr& node) = 0;
	virtual void visit(VarDeclExpr& node) = 0;
	virtual void visit(Block& node) = 0;
	virtual void visit(IfExpr& node) = 0;
	virtual void visit(WhileExpr& node) = 0;
	virtual void visit(LoopExpr& node) = 0;
	virtual void visit(MemberAccessExpr& node) = 0;
	virtual void visit(ArrayIndexExpr& node) = 0;
	virtual void visit(ArrayLiteralExpr& node) = 0;
	virtual void visit(TupleLiteralExpr& node) = 0;
	virtual void visit(YieldExpr& node) = 0;
	virtual void visit(BreakExpr& node) = 0;
	virtual void visit(ReturnExpr& node) = 0;
	virtual void visit(ContinueExpr& node) = 0;
	virtual void visit(ImportExpr& node) = 0;
	virtual void visit(FunctionDeclExpr& node) = 0;
};

//-------------------------------------------//
//-- Inline Accept Method Implementations ---//
//-------------------------------------------//

inline void LiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void IdentifierExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void BinaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void UnaryExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void PostfixExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void BorrowExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void CallExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void GenericCallExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void CastExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void AssignExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void VarDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Block::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void IfExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void WhileExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void LoopExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void MemberAccessExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ArrayIndexExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ArrayLiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void TupleLiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void YieldExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void BreakExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ReturnExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ContinueExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ImportExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void FunctionDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }

#endif // AST_HPP
