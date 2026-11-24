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

enum class DataType {
	I8, I16, I32, I64, Int,
	U8, U16, U32, U64, UInt,
	F32, F64, Float,
	Bool, Char, Str, Void, Custom
};

struct TypeDesc {
	DataType type = DataType::Void;
	std::optional<Token> customName;
	std::string customTypeName;
	std::vector<int> dim;

	bool isReference = false;
	bool isMutable   = false;
	bool isDynamic   = false;

	[[nodiscard]] bool isArray() const { return !dim.empty(); }

	[[nodiscard]] bool isSignedInteger() const {
		return type == DataType::I8 || type == DataType::I16 || type == DataType::I32 || 
			type == DataType::I64 || type == DataType::Int;
	}

	[[nodiscard]] bool isUnsignedInteger() const {
		return type == DataType::U8 || type == DataType::U16 || type == DataType::U32 || 
			type == DataType::U64 || type == DataType::UInt;
	}

	[[nodiscard]] bool isInteger() const {
		return isSignedInteger() || isUnsignedInteger();
	}

	[[nodiscard]] bool isFloat() const {
		return type == DataType::F32 || type == DataType::F64 || type == DataType::Float;
	}
};

//--------------------------//
//-- Base Node Definition --//
//--------------------------//

struct Expression {
	size_t line = 0;
	size_t col = 0;

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

//------------------------------//
//-- Structures & creations   --//
//------------------------------//

struct StructureField {
	std::string name;
	TypeDesc type;
	bool isReference = false;
	bool isMutable = false;
	std::optional<ExprPtr> defaultVal = std::nullopt;

	StructureField(std::string name, TypeDesc type, bool isReference = false, bool isMutable = false, std::optional<ExprPtr> defaultVal = std::nullopt)
		: name(std::move(name)), type(std::move(type)), isReference(isReference), isMutable(isMutable), defaultVal(std::move(defaultVal)) {}

	// Move-only due to unique_ptr in defaultVal
	StructureField(const StructureField&) = delete;
	StructureField& operator=(const StructureField&) = delete;
	StructureField(StructureField&&) noexcept = default;
	StructureField& operator=(StructureField&&) noexcept = default;

	[[nodiscard]] StructureField clone() const {
		return StructureField(name, type, isReference,isMutable, cloneOptional(defaultVal));
	}
};

struct StructCreateExpr : Expression {
	ExprPtr structObj;
	std::vector<ExprPtr> arguments;

	StructCreateExpr(ExprPtr structObj = nullptr, std::vector<ExprPtr> arguments = {})
		: structObj(std::move(structObj)), arguments(std::move(arguments)) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		return std::make_unique<StructCreateExpr>(structObj ? structObj->clone() : nullptr, cloneVector(arguments));
	}
};

struct StructDeclExpr : Expression {
	std::string name;
	std::vector<StructureField> attributes;
	bool isPublic = false;

	StructDeclExpr(std::string name = "", std::vector<StructureField> attributes = {}, bool isPublic = false)
		: name(std::move(name)), attributes(std::move(attributes)), isPublic(isPublic) {}

	void accept(ASTVisitor& visitor) override;
	[[nodiscard]] ExprPtr clone() const override {
		std::vector<StructureField> clonedFields;
		clonedFields.reserve(attributes.size());
		for (const auto& f : attributes) {
			clonedFields.push_back(f.clone());
		}

		return std::make_unique<StructDeclExpr>(name, std::move(clonedFields), isPublic);
	}
};

//----------------------//
//-- Enum & Matching  --//
//----------------------//

struct EnumField {
	std::string name;
	std::vector<StructureField> attributes; // Empty for unit variants like Error
	std::optional<int> explicitValue;       // e.g. 0, 1, 3; resolved to tag during sema

	EnumField(std::string name, std::vector<StructureField> attributes = {}, std::optional<int> explicitValue = std::nullopt)
		: name(std::move(name)), attributes(std::move(attributes)), explicitValue(explicitValue) {}

	// Move-only because StructureField contains unique_ptr (in defaultVal)
	EnumField(const EnumField&) = delete;
	EnumField& operator=(const EnumField&) = delete;
	EnumField(EnumField&&) noexcept = default;
	EnumField& operator=(EnumField&&) noexcept = default;

	[[nodiscard]] EnumField clone() const {
		std::vector<StructureField> clonedAttrs;
		clonedAttrs.reserve(attributes.size());
		for (const auto& attr : attributes) {
			clonedAttrs.push_back(attr.clone());
		}
		return EnumField(name, std::move(clonedAttrs), explicitValue);
	}
};

struct EnumDeclExpr : Expression {
	std::string name;
	std::vector<EnumField> fields;
	bool isPublic = false;

	EnumDeclExpr(std::string name = "", std::vector<EnumField> fields = {}, bool isPublic = false)
		: name(std::move(name)), fields(std::move(fields)), isPublic(isPublic) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		std::vector<EnumField> clonedFields;
		clonedFields.reserve(fields.size());
		for (const auto& f : fields) {
			clonedFields.push_back(f.clone());
		}
		auto copy = std::make_unique<EnumDeclExpr>(name, std::move(clonedFields), isPublic);
		copy->line = line;
		copy->col = col;
		return copy;
	}

	[[nodiscard]] bool requiresSemicolon() const override { return false; }
};

// Constructing a variant: e.g. Test::Error or Test::IsHuman(123)
struct EnumCreateExpr : Expression {
	std::string enumName;       // e.g. "Test" (or ExprPtr if qualified/path-based)
	std::string variantName;    // e.g. "Error" or "IsHuman"
	std::vector<ExprPtr> args;  // Payload arguments (empty for unit variants)

	EnumCreateExpr(std::string enumName, std::string variantName, std::vector<ExprPtr> args = {})
		: enumName(std::move(enumName)), variantName(std::move(variantName)), args(std::move(args)) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		auto copy = std::make_unique<EnumCreateExpr>(enumName, variantName, cloneVector(args));
		copy->line = line;
		copy->col = col;
		return copy;
	}
};

enum class EnumGetKind { NamedField, PositionalField };

struct EnumGetExpr : Expression {
	ExprPtr target;
	EnumGetKind kind = EnumGetKind::PositionalField;
	std::string fieldName;
	size_t fieldIndex = 0;
	std::optional<std::string> expectedVariant;

	EnumGetExpr(ExprPtr target, std::string fieldName, std::optional<std::string> expectedVariant = std::nullopt)
		: target(std::move(target)),
		  kind(EnumGetKind::NamedField),
		  fieldName(std::move(fieldName)),
		  expectedVariant(std::move(expectedVariant)) {}

	EnumGetExpr(ExprPtr target, size_t fieldIndex, std::optional<std::string> expectedVariant = std::nullopt)
		: target(std::move(target)),
		  kind(EnumGetKind::PositionalField),
		  fieldIndex(fieldIndex),
		  expectedVariant(std::move(expectedVariant)) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		auto copy = (kind == EnumGetKind::NamedField)
			? std::make_unique<EnumGetExpr>(target ? target->clone() : nullptr, fieldName, expectedVariant)
			: std::make_unique<EnumGetExpr>(target ? target->clone() : nullptr, fieldIndex, expectedVariant);
		copy->line = line;
		copy->col = col;
		return copy;
	}
};

// Pattern inside a match arm:
//   - Wildcard: `_`
//   - Variant pattern: `Error` or `IsHuman(test)` or `IsHuman(_)`
struct MatchPattern {
	enum class Kind { Wildcard, Variant };

	Kind kind = Kind::Wildcard;
	std::optional<std::string> enumName;
	std::string variantName;
	std::vector<std::string> bindingNames;

	static MatchPattern wildcard() {
		MatchPattern p;
		p.kind = Kind::Wildcard;
		return p;
	}

	static MatchPattern variant(std::string variantName, std::vector<std::string> bindingNames = {}, std::optional<std::string> enumName = std::nullopt) {
		MatchPattern p;
		p.kind = Kind::Variant;
		p.enumName = std::move(enumName);
		p.variantName = std::move(variantName);
		p.bindingNames = std::move(bindingNames);
		return p;
	}

	[[nodiscard]] MatchPattern clone() const {
		return MatchPattern{kind, enumName, variantName, bindingNames};
	}
};

struct MatchArm {
	MatchPattern pattern;
	std::optional<ExprPtr> guard;
	ExprPtr body;

	MatchArm(MatchPattern pattern, ExprPtr body, std::optional<ExprPtr> guard = std::nullopt)
		: pattern(std::move(pattern)), guard(std::move(guard)), body(std::move(body)) {}

	MatchArm(const MatchArm&) = delete;
	MatchArm& operator=(const MatchArm&) = delete;
	MatchArm(MatchArm&&) noexcept = default;
	MatchArm& operator=(MatchArm&&) noexcept = default;

	[[nodiscard]] MatchArm clone() const {
		return MatchArm(pattern.clone(), body ? body->clone() : nullptr, cloneOptional(guard));
	}
};

struct MatchExpr : Expression {
	ExprPtr scrutinee;          // Expression being matched, e.g. `t`
	std::vector<MatchArm> arms;

	MatchExpr(ExprPtr scrutinee = nullptr, std::vector<MatchArm> arms = {})
		: scrutinee(std::move(scrutinee)), arms(std::move(arms)) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		std::vector<MatchArm> clonedArms;
		clonedArms.reserve(arms.size());
		for (const auto& arm : arms) {
			clonedArms.push_back(arm.clone());
		}
		auto copy = std::make_unique<MatchExpr>(scrutinee ? scrutinee->clone() : nullptr, std::move(clonedArms));
		copy->line = line;
		copy->col = col;
		return copy;
	}

	[[nodiscard]] bool requiresSemicolon() const override { return false; }
};

// Switch for flat values (ints, chars, strings, tagless enums) with `_` catch-all
struct SwitchCase {
	bool isWildcard = false;
	std::vector<ExprPtr> labels;
	ExprPtr body;

	SwitchCase(bool isWildcard, std::vector<ExprPtr> labels, ExprPtr body)
		: isWildcard(isWildcard), labels(std::move(labels)), body(std::move(body)) {}

	SwitchCase(const SwitchCase&) = delete;
	SwitchCase& operator=(const SwitchCase&) = delete;
	SwitchCase(SwitchCase&&) noexcept = default;
	SwitchCase& operator=(SwitchCase&&) noexcept = default;

	[[nodiscard]] SwitchCase clone() const {
		return SwitchCase(isWildcard, cloneVector(labels), body ? body->clone() : nullptr);
	}
};

struct SwitchExpr : Expression {
	ExprPtr value;
	std::vector<SwitchCase> cases;

	SwitchExpr(ExprPtr value = nullptr, std::vector<SwitchCase> cases = {})
		: value(std::move(value)), cases(std::move(cases)) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		std::vector<SwitchCase> clonedCases;
		clonedCases.reserve(cases.size());
		for (const auto& c : cases) {
			clonedCases.push_back(c.clone());
		}
		auto copy = std::make_unique<SwitchExpr>(value ? value->clone() : nullptr, std::move(clonedCases));
		copy->line = line;
		copy->col = col;
		return copy;
	}

	[[nodiscard]] bool requiresSemicolon() const override { return false; }
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

struct ForExpr : Expression {
	ExprPtr itemDecl;
	ExprPtr iterable;
	ExprPtr body;

	ForExpr(ExprPtr itemDecl = nullptr, ExprPtr iterable = nullptr, ExprPtr body = nullptr)
		: itemDecl(std::move(itemDecl)),
		  iterable(std::move(iterable)),
		  body(std::move(body)) {}

	void accept(ASTVisitor& visitor) override;

	[[nodiscard]] ExprPtr clone() const override {
		auto cloned = std::make_unique<ForExpr>(
			itemDecl ? itemDecl->clone() : nullptr,
			iterable ? iterable->clone() : nullptr,
			body ? body->clone() : nullptr
		);
		cloned->line = line;
		cloned->col = col;
		return cloned;
	}

	[[nodiscard]] bool requiresSemicolon() const override { return false; }
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

struct ArrayAllocExpr : Expression {
	TypeDesc type;
	std::string name;
	std::optional<ExprPtr> initExpr;

	ArrayAllocExpr(TypeDesc type, std::string name, std::optional<ExprPtr> initExpr = std::nullopt)
		: type(std::move(type)), name(std::move(name)), initExpr(std::move(initExpr)) {}

	void accept(ASTVisitor& visitor) override;

	ExprPtr clone() const override {
		std::optional<ExprPtr> clonedInit;
		if (initExpr.has_value() && *initExpr) {
			clonedInit = (*initExpr)->clone();
		}
		auto copy = std::make_unique<ArrayAllocExpr>(type, name, std::move(clonedInit));
		copy->line = line;
		copy->col = col;
		return copy;
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
	[[nodiscard]] virtual bool requiresSemicolon() const override { return false; }
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
	virtual void visit(StructCreateExpr& node) = 0;
	virtual void visit(StructDeclExpr& node) = 0;
	virtual void visit(CastExpr& node) = 0;
	virtual void visit(AssignExpr& node) = 0;
	virtual void visit(VarDeclExpr& node) = 0;
	virtual void visit(Block& node) = 0;
	virtual void visit(IfExpr& node) = 0;
	virtual void visit(WhileExpr& node) = 0;
	virtual void visit(ForExpr& node) = 0;
	virtual void visit(LoopExpr& node) = 0;
	virtual void visit(MemberAccessExpr& node) = 0;
	virtual void visit(ArrayIndexExpr& node) = 0;
	virtual void visit(ArrayLiteralExpr& node) = 0;
	virtual void visit(ArrayAllocExpr& node) = 0;
	virtual void visit(TupleLiteralExpr& node) = 0;
	virtual void visit(YieldExpr& node) = 0;
	virtual void visit(BreakExpr& node) = 0;
	virtual void visit(ReturnExpr& node) = 0;
	virtual void visit(ContinueExpr& node) = 0;
	virtual void visit(ImportExpr& node) = 0;
	virtual void visit(FunctionDeclExpr& node) = 0;
	virtual void visit(EnumDeclExpr& node) = 0;
	virtual void visit(EnumCreateExpr& node) = 0;
	virtual void visit(EnumGetExpr& node) = 0;
	virtual void visit(MatchExpr& node) = 0;
	virtual void visit(SwitchExpr& node) = 0;
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
inline void StructCreateExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void StructDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void CastExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void AssignExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void VarDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void Block::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void IfExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void WhileExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ForExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void LoopExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void MemberAccessExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ArrayIndexExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ArrayLiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ArrayAllocExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void TupleLiteralExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void YieldExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void BreakExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ReturnExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ContinueExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void ImportExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void FunctionDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void EnumDeclExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void EnumCreateExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void EnumGetExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void MatchExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }
inline void SwitchExpr::accept(ASTVisitor& visitor) { visitor.visit(*this); }

#endif // AST_HPP
