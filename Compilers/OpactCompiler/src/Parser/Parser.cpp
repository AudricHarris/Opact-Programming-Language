#include "CompilerPipeline.hpp"
#include "../Parser/Parser.hpp"
#include "../Parser/Ast.hpp"
#include "../Token/TokenType.hpp"
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

//----------------------//
//-- Token Navigation --//
//----------------------//

const Token &Parser::peek() const {
	if (this->idx >= this->tokens.size()) {
		static const Token EOF_TOKEN{TokenKind::END_OF_FILE, "", 0, 0};
		return EOF_TOKEN;
	}
	return this->tokens[this->idx];
}

const Token &Parser::peekAt(size_t offset) const {
	size_t idx = this->idx + offset;
	if (idx >= this->tokens.size()) {
		static const Token EOF_TOKEN{TokenKind::END_OF_FILE, "", 0, 0};
		return EOF_TOKEN;
	}
	return this->tokens[idx];
}

const Token Parser::consume() {
	if (this->idx >= this->tokens.size()) {
		static const Token EOF_TOKEN{TokenKind::END_OF_FILE, "", 0, 0};
		return EOF_TOKEN;
	}
	return this->tokens[this->idx++];
}

bool Parser::match(TokenKind k) {
	if (peek().getKind() == k) {
		consume();
		return true;
	}
	return false;
}

bool Parser::check(TokenKind kind) const {
	return !this->isAtEnd() && this->peek().getKind() == kind;
}

bool Parser::isAtEnd() const {
	return this->idx >= this->tokens.size() ||
		this->peek().getKind() == TokenKind::END_OF_FILE;
}

//--------------------//
//-- Error handling --//
//--------------------//

std::string Parser::generateError(TokenKind kind, std::string errorMsg)
{
	std::string formattedError = "\033[31m[Parse Error] Line " + 
		std::to_string(peek().getLine()) + ":" + 
		std::to_string(peek().getColumn()) + 
		" - " + errorMsg + "\033[0m\n";

	return formattedError;
}

Token Parser::expect(TokenKind kind, std::string_view errorMsg) {
	if (peek().getKind() == kind)
		return consume();
	std::string msg;
	if (errorMsg.empty()) {
		Token tmp(kind, "", 0, 0);
		msg = "Expected: " + tmp.toString() + ", got: `" + peek().getWord() + "`";
	} else {
		msg = std::string(errorMsg);
	}

	std::string formattedError = this->generateError(kind, msg); 

	std::cout << formattedError << "\n";
	throw std::runtime_error(formattedError);
}

void Parser::synchronize() {
	consume();
	while (!isAtEnd()) {
		if (peek().getKind() == TokenKind::SEMI) {
			consume();
			return;
		}
		switch (peek().getKind()) {
			case TokenKind::RETURN:
				return;
			case TokenKind::LBRACE:
				consume();
				return;
			default:
				consume();
		}
	}
}

//--------------------//
//-- Parsing module --//
//--------------------//

template<typename T, typename... Args>
ExprPtr makeNode(const Token& tok, Args&&... args) {
	auto node = std::make_unique<T>(std::forward<Args>(args)...);
	node->line = tok.getLine();
	node->col = tok.getColumn();
	return node;
}

ExprPtr Parser::parseModule()
{
	auto moduleBlock = std::make_unique<Block>();

	while (!this->isAtEnd())
	{
		try {
			ExprPtr item = this->parseTopLevel();
			if (item != nullptr)
				moduleBlock->expressions.push_back(std::move(item));
		}
		catch (const std::exception& e)
		{
			this->synchronize();
		}
	}

	return moduleBlock;
}

ExprPtr Parser::parseTopLevel()
{
	std::optional<Token> visibility;
	if (this->check(TokenKind::PUBLIC) || this->check(TokenKind::PRIVATE))
		visibility = this->consume();

	if (this->check(TokenKind::IMPORT))
	{
		return this->parseImport();
	}

	if (this->check(TokenKind::FN))
	{
		return this->parseFunction(visibility); 
	}

	if (this->check(TokenKind::STRUCT))
	{
		return this->parseStruct(visibility); 
	}

	this->consume();
	return nullptr;
}

//--------------------//
//-- Parsing Import --//
//--------------------//

void Parser::addModule(std::vector<std::string> path)
{
	std::string actualPath = "";
	for (const auto& word : path)
	{
		actualPath.append(word);
		actualPath.append("/");
	}

	if (!actualPath.empty())
		actualPath.erase(actualPath.size() - 1);

	actualPath.append(".op");

	this->importFiles.push_back(actualPath);
	this->pipeline->enqueueFile(actualPath, this->filePath);
}

ExprPtr Parser::parseImport()
{
	this->consume(); // consume TokenKind::IMPORT

	ImportPath path;
	std::vector<std::string> importedSymbols;
	bool isSelective = false;

	Token first = this->expect(TokenKind::IDENTIFIER, "Expected Module name");
	path.segments.push_back(first.getWord());
	path.isStdLib = (first.getWord() == "Opact" || first.getWord() == "Std");

	while (this->check(TokenKind::COLON_COLON))
	{
		this->consume();
		if (this->check(TokenKind::LBRACE))
		{
			this->consume();
			isSelective = true;
			if (!this->check(TokenKind::RBRACE))
			{
				do {
					Token sym = this->expect(TokenKind::IDENTIFIER, "Expected symbol name");
					importedSymbols.push_back(sym.getWord());
				} while (this->match(TokenKind::COMMA));
			}
			this->expect(TokenKind::RBRACE, "Expected '}'");
			break;
		}

		Token seg = this->expect(TokenKind::IDENTIFIER, "Expected module path segment");
		path.segments.push_back(seg.getWord());
	}

	this->addModule(path.segments);
	this->expect(TokenKind::SEMI, "Expected ';' after import");
	return makeNode<ImportExpr>(first, std::move(path), std::move(importedSymbols), isSelective);
}

DataType determineType(const Token& t)
{
	const std::string& word = t.getWord();

	if (word == "char") return DataType::Char;
	if (word == "str")  return DataType::Str;
	if (word == "bool") return DataType::Bool;
	if (word == "void") return DataType::Void;

	// Signed integers
	if (word == "i8")  return DataType::I8;
	if (word == "i16") return DataType::I16;
	if (word == "i32" || word == "int") return DataType::I32;
	if (word == "i64") return DataType::I64;

	// Unsigned integers
	if (word == "u8")  return DataType::U8;
	if (word == "u16") return DataType::U16;
	if (word == "u32" || word == "uint") return DataType::U32;
	if (word == "u64") return DataType::U64;

	// Floats
	if (word == "f32") return DataType::F32;
	if (word == "f64" || word == "float") return DataType::F64;

	return DataType::Custom;
}

//-----------------------//
//-- Parsing Structure --//
//-----------------------//

ExprPtr Parser::parseStruct(std::optional<Token> visib)
{
	this->consume(); // TokenKind::STRUCT

	bool isPublic = false;
	if (visib.has_value() && visib->getKind() == TokenKind::PUBLIC)
	{
		isPublic = true;
	}

	Token nameTok = this->expect(TokenKind::IDENTIFIER, "Expected identifier as name");
	std::string name = nameTok.getWord();

	this->expect(TokenKind::LBRACE, "Expected '{' for opening structure body");

	std::vector<StructureField> fields;
	do
	{
		bool isMut = false, isRef = false;
		if (this->check(TokenKind::AND))
		{
			isRef = true;
			this->consume();
			if (this->check(TokenKind::MUT))
			{
				isMut = true;
				this->consume();
			}
		}

		Token ptypeTok = this->expect(TokenKind::IDENTIFIER, "Expected type for the param");
		Token pnameTok = this->expect(TokenKind::IDENTIFIER, "Expected name for the param");

		std::optional<ExprPtr> defaultVal = std::nullopt;
		if (this->match(TokenKind::ASSIGN))
		{
			defaultVal = this->parseExpression();
		}

		TypeDesc ptype;
		ptype.type = determineType(ptypeTok);
		ptype.customTypeName = ptypeTok.getWord();
		ptype.isReference = isRef;
		ptype.isMutable = isMut;

		this->consume(); // Point virgule
		fields.emplace_back(pnameTok.getWord(), std::move(ptype), isRef, isMut, std::move(defaultVal));
	}
	while (!this->match(TokenKind::RBRACE));

	return makeNode<StructDeclExpr>(nameTok, std::move(name), std::move(fields), isPublic);
}

//----------------------//
//-- Parsing Function --//
//----------------------//
bool Parser::parseIsVarDeclRef()
{
	size_t offset = 1;

	if (this->peekAt(offset).getKind() == TokenKind::MUT) offset++;

	bool hasType = (this->peekAt(offset).getKind() == TokenKind::IDENTIFIER);
	if (!hasType) return false;
	offset++;

	while (this->peekAt(offset).getKind() == TokenKind::LBRACKET)
	{
		offset++;

		while (this->peekAt(offset).getKind() != TokenKind::RBRACKET && 
				this->peekAt(offset).getKind() != TokenKind::END_OF_FILE)
		{
			offset++;
		}

		if (this->peekAt(offset).getKind() == TokenKind::RBRACKET)
		{
			offset++;
		}
	}

	bool hasName = (this->peekAt(offset).getKind() == TokenKind::IDENTIFIER);

	return hasName;
}

bool Parser::parseIsVarDecl()
{
	int index = 0;

	// 1. The "type" is an identifier (e.g., int, i32, str, Human)
	if (this->peekAt(index).getKind() != TokenKind::IDENTIFIER) {
		return false;
	}
	index++;

	// 2. Optional: handle array brackets if they are part of the type (e.g., int[] or int[10])
	while (this->peekAt(index).getKind() == TokenKind::LBRACKET) {
		index++;
		while (this->peekAt(index).getKind() != TokenKind::RBRACKET && 
				this->peekAt(index).getKind() != TokenKind::END_OF_FILE) {
			index++;
		}
		if (this->peekAt(index).getKind() == TokenKind::RBRACKET) {
			index++;
		}
	}

	// 3. The variable name must also be an identifier (e.g., agePtr, h)
	if (this->peekAt(index).getKind() != TokenKind::IDENTIFIER) {
		return false;
	}
	index++;

	// 4. Confirm it's a declaration by checking for an assignment '=' or statement terminator ';'
	TokenKind nextKind = this->peekAt(index).getKind();
	return (nextKind == TokenKind::ASSIGN || nextKind == TokenKind::SEMI);
}

ExprPtr Parser::parseFunction(std::optional<Token> visib)
{
	this->consume(); // consume TokenKind::FN

	bool isPublic = false;
	if (visib.has_value() && visib->getKind() == TokenKind::PUBLIC)
	{
		isPublic = true;
	}

	Token nameTok = this->expect(TokenKind::IDENTIFIER, "Expected identifier as name");
	std::string name = nameTok.getWord();

	std::vector<Parameter> params;
	this->expect(TokenKind::LPAREN, "Expected a '(' for opening function params");

	if (!this->check(TokenKind::RPAREN))
	{
		do
		{
			bool isMut = false, isRef = false;
			if (this->check(TokenKind::AND))
			{
				isRef = true;
				this->consume();
				if (this->check(TokenKind::MUT))
				{
					isMut = true;
					this->consume();
				}
			}

			Token ptypeTok = this->expect(TokenKind::IDENTIFIER, "Expected type for the param");
			Token pnameTok = this->expect(TokenKind::IDENTIFIER, "Expected name for the param");

			std::optional<ExprPtr> defaultVal = std::nullopt;
			if (this->match(TokenKind::ASSIGN))
			{
				defaultVal = this->parseExpression();
			}

			TypeDesc ptype;
			ptype.type = determineType(ptypeTok);
			ptype.customTypeName = ptypeTok.getWord();
			ptype.isReference = isRef;
			ptype.isMutable = isMut;

			params.emplace_back(pnameTok.getWord(), std::move(ptype), isMut, isRef, std::move(defaultVal));
		}
		while (this->match(TokenKind::COMMA));
	}

	this->expect(TokenKind::RPAREN, "Expected a ')' for closing function params");

	TypeDesc returnType;
	returnType.type = DataType::Void;

	if (this->match(TokenKind::RETURN_TYPE))
	{
		bool isMut = false, isRef = false;
		if (this->check(TokenKind::AND))
		{
			isRef = true;
			this->consume();
			if (this->check(TokenKind::MUT))
			{
				isMut = true;
				this->consume();
			}
		}

		Token retTypeTok = this->expect(TokenKind::IDENTIFIER, "Expected a return type for the function");
		returnType.type = determineType(retTypeTok);
		returnType.customTypeName = retTypeTok.getWord();
		returnType.isReference = isRef;
		returnType.isMutable = isMut;
	}

	ExprPtr body = this->parseBlock();

	return makeNode<FunctionDeclExpr>(
			nameTok,
			std::move(name),
			std::move(params),
			std::move(returnType),
			std::move(body),
			isPublic
			);
}

//------------------//
//-- Parsing body --//
//------------------//

ExprPtr Parser::parseBlock()
{
	std::vector<ExprPtr> expressions;

	if (this->match(TokenKind::LBRACE))
	{
		while (!this->check(TokenKind::RBRACE) && !this->isAtEnd())
		{
			try {
				// Parse statement/expression
				ExprPtr expr = this->parseExpression();


				if (expr != nullptr) {
					bool requiresSemi = expr->requiresSemicolon();

					expressions.push_back(std::move(expr));

					if (requiresSemi && !this->check(TokenKind::RBRACE)) {
						this->expect(TokenKind::SEMI, "Expected ';' after statement");
					}
				}
			} catch (const std::exception& e) {
				this->synchronize();
			}
		}
		this->expect(TokenKind::RBRACE, "Expected '}' at end of block");
	}
	else
	{
		try {
			ExprPtr expr = this->parseExpression();
			if (expr != nullptr)
				expressions.push_back(std::move(expr));

			this->expect(TokenKind::SEMI, "Expected ';' after statement");
		} catch (const std::exception& e) {
			this->synchronize();
		}
	}

	return std::make_unique<Block>(std::move(expressions));
}

//------------------------//
//-- Parsing expression --//
//------------------------//

ExprPtr Parser::parseExpression()
{
	if (this->check(TokenKind::IF))
		return this->parseIf();

	if (this->check(TokenKind::RETURN))
		return this->parseReturn();

	if (this->check(TokenKind::WHILE))
		return this->parseWhile();

	if (this->check(TokenKind::LOOP))
		return this->parseLoop();

	if (this->check(TokenKind::BREAK)) {
		Token tok = this->consume();
		return makeNode<BreakExpr>(tok);
	}

	if (this->check(TokenKind::CONTINUE)) {
		Token tok = this->consume();
		return makeNode<ContinueExpr>(tok);
	}

	if (this->check(TokenKind::AND)) {
		if (this->parseIsVarDeclRef())
			return this->parseVarDecl();
		return this->parseBorrow();
	}

	if (this->parseIsVarDecl()) {
		return this->parseVarDecl();
	}

	return this->parseAssignement();
}

//---------------------------//
//-- Parsing If expression --//
//---------------------------//

ExprPtr Parser::parseIf()
{
	Token tok = this->peek();
	this->expect(TokenKind::IF, "Expected 'if' at the start of an if expression");
	this->expect(TokenKind::LPAREN, "Expected '(' for opening condition");
	ExprPtr condition = this->parseExpression();
	this->expect(TokenKind::RPAREN, "Expected ')' for closing condition");

	ExprPtr ifBranch = this->parseBlock(); 
	std::optional<ExprPtr> elseBranch = std::nullopt;

	if (this->match(TokenKind::ELSE)) {
		if (this->check(TokenKind::IF)) {
			elseBranch = this->parseIf();
		} else {
			elseBranch = this->parseBlock(); 
		}
	}

	return makeNode<IfExpr>(tok, std::move(condition), std::move(ifBranch), std::move(elseBranch));
}

//------------------------------//
//-- Parsing While expression --//
//------------------------------//

ExprPtr Parser::parseWhile()
{
	Token tok = this->peek();
	this->expect(TokenKind::WHILE, "Expected 'while' at the start of a while expression");
	this->expect(TokenKind::LPAREN, "Expected '(' for opening condition");
	ExprPtr condition = this->parseExpression();
	this->expect(TokenKind::RPAREN, "Expected ')' for closing condition");

	ExprPtr loopBranch = this->parseBlock(); 

	return makeNode<WhileExpr>(tok, std::move(condition), std::move(loopBranch));
}


ExprPtr Parser::parseLoop()
{
	Token tok = this->peek();
	this->expect(TokenKind::LOOP, "Expected 'loop' keyword");
	ExprPtr body = this->parseBlock();
	return makeNode<LoopExpr>(tok, std::move(body));
}

//---------------------//
//-- Parsing Fn Call --//
//---------------------//

ExprPtr Parser::parseCallExpr(ExprPtr callee)
{
	Token tok = this->peekAt(-2);
	std::vector<ExprPtr> args;

	if (!this->check(TokenKind::RPAREN)) {
		do {
			args.push_back(this->parseExpression());
		} while (this->match(TokenKind::COMMA));
	}

	this->expect(TokenKind::RPAREN, "Expected ')' after function arguments");

	return makeNode<CallExpr>(tok ,std::move(callee), std::move(args));
}

//-------------------------//
//-- Parsing Return expr --//
//-------------------------//

ExprPtr Parser::parseReturn()
{
	Token tok = this->peek();
	this->consume(); // consume TokenKind::RETURN
	std::optional<ExprPtr> value = std::nullopt;

	if (!this->check(TokenKind::SEMI))
		value = this->parseExpression();

	return makeNode<ReturnExpr>(tok, std::move(value));
}

//---------------------------//
//-- Parsing Variable Decl --//
//---------------------------//

ExprPtr Parser::parseVarDecl()
{
	Token startTok = this->peek();
	TypeDesc desc;

	// Handle reference types (& / &mut)
	if (this->match(TokenKind::AND))
	{
		desc.isReference = true;
		if (this->match(TokenKind::MUT))
		{
			desc.isMutable = true;
		}
	}

	// Parse Base Type Identifier (e.g. i32, bool, MyClass)
	Token typeTok = this->expect(TokenKind::IDENTIFIER, "Expected type name for variable declaration");
	desc.type = determineType(typeTok); 
	desc.customTypeName = typeTok.getWord();

	// Parse Array Dimensions [5][10]...
	while (this->match(TokenKind::LBRACKET))
	{
		Token sizeTok = this->expect(TokenKind::LIT_INT, "Expected integer dimension size inside brackets []");

		// Convert token value to integer (assuming getInt() or std::stoi)
		int dimensionSize = std::stoi(sizeTok.getWord()); 
		desc.dim.push_back(dimensionSize);

		this->expect(TokenKind::RBRACKET, "Expected closing ']' after array size");
	}

	// Parse Variable Name Identifier (e.g. t, x, my_var)
	Token nameTok = this->expect(TokenKind::IDENTIFIER, "Expected variable name");

	// Parse Optional Initializer (= ...)
	ExprPtr initExpr = nullptr;
	if (this->match(TokenKind::ASSIGN))
	{
		initExpr = this->parseExpression();
	}

	return makeNode<VarDeclExpr>(startTok, nameTok, std::move(desc), std::move(initExpr));
}

ExprPtr Parser::parseBorrow()
{
	Token tok = this->peek();
	this->consume(); // Removal of &
	bool isMut = this->match(TokenKind::MUT); 

	auto expr = this->parseExpression();

	return makeNode<BorrowExpr>(tok, std::move(expr), isMut);

}

ExprPtr Parser::parseAssignement()
{
	ExprPtr target = this->parseOr();

	Token symbolAssign = this->peek(); 

	switch (symbolAssign.getKind()) {
		case TokenKind::ASSIGN: {
									Token assignTok = this->consume();
									ExprPtr val = this->parseAssignement();
									return makeNode<AssignExpr>(assignTok, std::move(target), std::move(val), AssignKind::Assign);
								}

		case TokenKind::ADD_ASSIGN:
		case TokenKind::SUB_ASSIGN:
		case TokenKind::MUL_ASSIGN:
		case TokenKind::DIV_ASSIGN: {
										Token opTok = this->consume();

										BinaryOp op;
										switch (symbolAssign.getKind()) {
											case TokenKind::ADD_ASSIGN: op = BinaryOp::Add; break;
											case TokenKind::SUB_ASSIGN: op = BinaryOp::Sub; break;
											case TokenKind::MUL_ASSIGN: op = BinaryOp::Mul; break;
											case TokenKind::DIV_ASSIGN: op = BinaryOp::Div; break;
											default: break;
										}

										ExprPtr val = this->parseAssignement();

										ExprPtr rhs = std::make_unique<BinaryExpr>(op, target->clone(), std::move(val));
										return makeNode<AssignExpr>(opTok, std::move(target), std::move(rhs), AssignKind::Assign); 
									}

		default:
									return target;
	}
}

ExprPtr Parser::parseOr()
{
	auto expr = this->parseAnd();
	while (this->match(TokenKind::OR))
		expr = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(expr), this->parseAnd());

	return expr;
}

ExprPtr Parser::parseAnd()
{
	auto expr = this->parseEquality();
	while (this->check(TokenKind::AND) || this->check(TokenKind::DOUBLE_AND))
	{
		BinaryOp op = BinaryOp::And;
		if (this->match(TokenKind::DOUBLE_AND)) 
			op = BinaryOp::BitAnd;
		else 
			this->consume();

		expr = std::make_unique<BinaryExpr>(op, std::move(expr), this->parseEquality());
	}

	return expr;
}

ExprPtr Parser::parseEquality()
{
	auto expr = this->parseComparison();
	while (this->check(TokenKind::EQ) || this->check(TokenKind::NE))
	{
		BinaryOp op = BinaryOp::Eq;
		if (this->match(TokenKind::NE)) 
			op = BinaryOp::Ne;
		else 
			this->consume();

		expr = std::make_unique<BinaryExpr>(op, std::move(expr), this->parseComparison());
	}

	return expr;
}

static bool peekRelOp(TokenKind k, BinaryOp &op) {
	switch (k) {
		case TokenKind::LT:
			op = BinaryOp::Lt;
			return true;
		case TokenKind::GT:
			op = BinaryOp::Gt;
			return true;
		case TokenKind::LE:
			op = BinaryOp::Le;
			return true;
		case TokenKind::GE:
			op = BinaryOp::Ge;
			return true;
		default:
			return false;
	}
}

ExprPtr Parser::parseComparison() {
	auto expr = this->parseAdditive();

	BinaryOp op;
	while (peekRelOp(this->peek().getKind(), op)) {
		this->consume();
		auto rhs = this->parseAdditive();
		expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(rhs));
	}

	return expr;
}

ExprPtr Parser::parseAdditive()
{
	auto expr = this->parseMultiplicative();

	while (this->check(TokenKind::ADD) || this->check(TokenKind::SUB))
	{
		BinaryOp op = BinaryOp::Add;
		if (this->match(TokenKind::SUB)) 
			op = BinaryOp::Sub;
		else 
			this->consume();

		auto rhs = this->parseMultiplicative();
		expr = std::make_unique<BinaryExpr>(op, std::move(expr), std::move(rhs));
	}

	return expr;
}

ExprPtr Parser::parseMultiplicative()
{
	auto expr = this->parseUnary();
	while (true)
	{
		BinaryOp op;

		if (this->match(TokenKind::PROD))
			op = BinaryOp::Mul;
		else if (this->match(TokenKind::POWER))
			op = BinaryOp::Power;
		else if (this->match(TokenKind::DIV))
			op = BinaryOp::Div;
		else if (this->match(TokenKind::MOD))
			op = BinaryOp::Mod;
		else
			break;

		expr = std::make_unique<BinaryExpr>(op, std::move(expr), this->parseUnary());
	}

	return expr;
}

//----------------------------------//
//-- Primary & Postfix Expression --//
//----------------------------------//

ExprPtr Parser::parseUnary()
{
	if (this->match(TokenKind::NOT))
		return std::make_unique<UnaryExpr>(UnaryOp::Not, this->parseUnary());
	if (this->match(TokenKind::SUB))
		return std::make_unique<UnaryExpr>(UnaryOp::Negate, this->parseUnary());

	if (this->match(TokenKind::AND)) {
		bool isMut = this->match(TokenKind::MUT);
		return std::make_unique<BorrowExpr>(this->parseUnary(), isMut);
	}

	ExprPtr expr = this->parsePrimary();
	return this->parsePostfix(std::move(expr));
}

ExprPtr Parser::parsePostfix(ExprPtr expr)
{
	while (true) {
		// 1. Array Indexing
		if (this->check(TokenKind::LBRACKET)) {
			std::vector<ExprPtr> indices;
			do {
				this->consume(); // Consume '['
				indices.push_back(this->parseExpression());
				this->expect(TokenKind::RBRACKET, "Expected ']' after array index");
			} while (this->check(TokenKind::LBRACKET));

			expr = std::make_unique<ArrayIndexExpr>(std::move(expr), std::move(indices));
			continue;
		}

		// 2. Member Access
		if (this->match(TokenKind::DOT)) {
			Token prop = this->expect(TokenKind::IDENTIFIER, "Expected property or member name after '.'");
			expr = std::make_unique<MemberAccessExpr>(std::move(expr), prop.getWord());
			continue;
		}

		// 3. Function Call
		if (this->match(TokenKind::LPAREN)) {
			expr = this->parseCallExpr(std::move(expr));
			continue;
		}
		// 4. Structure/Class defining
		if (this->match(TokenKind::LBRACE)){
			std::vector<ExprPtr> args;
			if (!this->check(TokenKind::RBRACE))
			{
				do {
					args.push_back(this->parseExpression());
				} while (this->match(TokenKind::COMMA));
			}
			this->expect(TokenKind::RBRACE, "Expected '}' after structure arguments");
			expr = std::make_unique<StructCreateExpr>(std::move(expr), std::move(args));
			continue;
		}

		// 5. Postfix Increment
		if (this->match(TokenKind::INCREMENT)) {
			expr = std::make_unique<PostfixExpr>(PostfixOp::Increment, std::move(expr));
			continue;
		}

		// 6. Postfix Decrement
		if (this->match(TokenKind::DECREMENT)) {
			expr = std::make_unique<PostfixExpr>(PostfixOp::Decrement, std::move(expr));
			continue;
		}

		// 7. Type Casting
		if (this->match(TokenKind::AS)) {
			Token castTypeTok = this->expect(TokenKind::IDENTIFIER, "Expected target type after 'as'");

			TypeDesc targetType;
			targetType.type = determineType(castTypeTok);
			targetType.customTypeName = castTypeTok.getWord();

			expr = std::make_unique<CastExpr>(std::move(expr), std::move(targetType));
			continue;
		}

		break;
	}

	return expr;
}

ExprPtr Parser::parsePrimary()
{
	if (this->match(TokenKind::LPAREN)) {
		auto expr = this->parseExpression();
		this->expect(TokenKind::RPAREN, "Expected ')' after parenthesized expression");
		return expr;
	}

	if (this->check(TokenKind::IDENTIFIER)) {
		Token tok = this->consume();
		return std::make_unique<IdentifierExpr>(tok.getWord());
	}

	if (this->check(TokenKind::LBRACE))
	{
		return this->parseArrayLiteral();
	}

	Token tok = this->consume();
	switch (tok.getKind()) {
		case TokenKind::LIT_INT:
			{
				std::string number = tok.getWord();
				NumericBase type = NumericBase::Decimal;
				if (number.rfind("0x", 0) == 0)
					type = NumericBase::Hexadecimal;
				else if (number.rfind("0b", 0) == 0)
					type = NumericBase::Binary;
				else if (number.rfind("0o", 0) == 0)
					type = NumericBase::Octal;

				return std::make_unique<LiteralExpr>(LiteralKind::Int, tok, type);
			}

		case TokenKind::LIT_CHAR:
			return std::make_unique<LiteralExpr>(LiteralKind::Char, tok);

		case TokenKind::LIT_FLOAT:
			return std::make_unique<LiteralExpr>(LiteralKind::Float, tok);

		case TokenKind::LIT_BOOL:
			return std::make_unique<LiteralExpr>(LiteralKind::Bool, tok);

		case TokenKind::LIT_STRING:
			{
				std::string merged = tok.getWord();
				while (this->check(TokenKind::LIT_STRING))
				{
					Token str = this->consume();
					merged += str.getWord();
				}

				Token res{TokenKind::LIT_STRING, merged, tok.getLine(), tok.getColumn()};
				return std::make_unique<LiteralExpr>(LiteralKind::Str, res);
			}

		default: {
					 std::string error = this->generateError(
							 tok.getKind(), 
							 "Unexpected token '" + tok.getWord() + "'"
							 );
					 std::cerr << error;
					 throw std::runtime_error(error);
				 }
	}
}

ExprPtr Parser::parseArrayLiteral()
{
	Token tok = this->expect(TokenKind::LBRACE, "Expected '{' to start array initializer");
	std::vector<ExprPtr> elements;

	// Handle non-empty initializer list
	if (!this->check(TokenKind::RBRACE))
	{
		do
		{
			// Allow trailing commas (e.g., {1, 2, 3,})
			if (this->check(TokenKind::RBRACE))
			{
				break;
			}

			elements.push_back(this->parseExpression());

		} while (this->match(TokenKind::COMMA));
	}

	this->expect(TokenKind::RBRACE, "Expected '}' after array initializer list");

	return makeNode<ArrayLiteralExpr>(tok, std::move(elements));
}
