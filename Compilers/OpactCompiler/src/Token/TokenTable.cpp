#include "TokenType.hpp"
#include <string>

// This only serves for debugging //
std::string Token::toString() {
	switch (this->kind) {
		case TokenKind::IDENTIFIER:
			return "IDENTIFIER	";
		case TokenKind::IF:
			return "IF	";
		case TokenKind::ELSE:
			return "ELSE  ";
		case TokenKind::WHILE:
			return "WHILE  ";
		case TokenKind::LOOP:
			return "LOOP\n";
		case TokenKind::FOR:
			return "FOR  ";
		case TokenKind::RETURN:
			return "RETURN	";
		case TokenKind::AS:
			return "AS	";
		case TokenKind::FN:
			return "FN	";
		case TokenKind::MUT:
			return "MUT  ";
		case TokenKind::CONST:
			return "CONST  ";
		case TokenKind::IMPORT:
			return "IMPORT	";
		case TokenKind::PUBLIC:
			return "PUBLIC	";
		case TokenKind::PRIVATE:
			return "PRIVATE  ";
		case TokenKind::PROTECTED:
			return "PROTECTED  ";
		case TokenKind::CONTINUE:
			return "CONTINUE  ";
		case TokenKind::BREAK:
			return "BREAK  ";
		case TokenKind::ENUM:
			return "ENUM  ";
		case TokenKind::MATCH:
			return "MATCH  ";
		case TokenKind::LIT_INT:
			return "LIT_INT  ";
		case TokenKind::LIT_FLOAT:
			return "LIT_FLOAT  ";
		case TokenKind::LIT_STRING:
			return "LIT_STRING	";
		case TokenKind::LIT_CHAR:
			return "LIT_CHAR  ";
		case TokenKind::LIT_BOOL:
			return "LIT_BOOL  ";
		case TokenKind::ASSIGN:
			return "ASSIGN	";
		case TokenKind::INCREMENT:
			return "INCREMENT  ";
		case TokenKind::DECREMENT:
			return "DECREMENT  ";
		case TokenKind::BORROW:
			return "BORROW	";
		case TokenKind::ADD:
			return "ADD  ";
		case TokenKind::SUB:
			return "SUB  ";
		case TokenKind::PROD:
			return "PROD  ";
		case TokenKind::POWER:
			return "POWER  ";
		case TokenKind::DIV:
			return "DIV  ";
		case TokenKind::MOD:
			return "MOD  ";
		case TokenKind::LT:
			return "LT	";
		case TokenKind::GT:
			return "GT	";
		case TokenKind::LE:
			return "LE	";
		case TokenKind::GE:
			return "GE	";
		case TokenKind::EQ:
			return "EQ	";
		case TokenKind::NE:
			return "NE	";
		case TokenKind::AND:
			return "AND  ";
		case TokenKind::DOUBLE_AND:
			return "DOUBLE_AND	";
		case TokenKind::OR:
			return "OR	";
		case TokenKind::NOT:
			return "NOT  ";
		case TokenKind::ADD_ASSIGN:
			return "ADD_ASSIGN	";
		case TokenKind::SUB_ASSIGN:
			return "SUB_ASSIGN	";
		case TokenKind::MUL_ASSIGN:
			return "MUL_ASSIGN	";
		case TokenKind::DIV_ASSIGN:
			return "DIV_ASSIGN	";
		case TokenKind::DIVF_ASSIGN:
			return "DIVF_ASSIGN  ";
		case TokenKind::LPAREN:
			return "LPAREN	";
		case TokenKind::RPAREN:
			return "RPAREN	";
		case TokenKind::LBRACKET:
			return "LBRACKET  ";
		case TokenKind::RBRACKET:
			return "RBRACKET  ";
		case TokenKind::LBRACE:
			return "\nLBRACE\n";
		case TokenKind::RBRACE:
			return "RBRACE\n";
		case TokenKind::COMMA:
			return "COMMA  ";
		case TokenKind::SEMI:
			return "SEMI\n";
		case TokenKind::DOT:
			return "DOT  ";
		case TokenKind::COLON:
			return "COLON  ";
		case TokenKind::COLON_COLON:
			return "COLON_COLON  ";
		case TokenKind::RETURN_TYPE:
			return "RETURN_TYPE  ";
		case TokenKind::NEW:
			return "NEW  ";
		case TokenKind::END_OF_FILE:
			return "EOF\n\n";
		case TokenKind::COMMENT:
			return "COMMENT  ";
		case TokenKind::UNKNOWN:
			return "UNKNOWN  ";
		case TokenKind::NUM_TOKENS:
			return "NUM_TOKENS	";
		default:
			return "??? (invalid kind)";
	}
}
