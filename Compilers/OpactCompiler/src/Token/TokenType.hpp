/**
 * @file TokenType.hpp
 * @brief Token types and representation for the compiler lexer.
 */

#ifndef TOKEN_TYPE_HPP
#define TOKEN_TYPE_HPP

#include <string>
#include <string_view>

/**
 * @enum TokenKind
 * @brief Enumeration of all token categories in the language.
 */
enum class TokenKind {
	// KeyWords
	IDENTIFIER,
	IF,
	ELSE,
	WHILE,
	LOOP,
	FOR,
	RETURN,
	AS,
	FN,
	MUT,
	CONST,
	IMPORT,
	PUBLIC,
	PRIVATE,
	PROTECTED,
	CONTINUE,
	BREAK,
	ENUM,
	MATCH,

	// Literals
	LIT_INT,
	LIT_FLOAT,
	LIT_STRING,
	LIT_CHAR,
	LIT_BOOL,

	// Operations
	ASSIGN,
	INCREMENT,
	DECREMENT,
	FAT_ARROW,
	BORROW,
	ADD,
	SUB,
	PROD,
	POWER,
	DIV,
	MOD,
	LT,  // <
	GT,  // >
	LE,  // <=
	GE,  // >=
	EQ,  // ==
	NE,  // !=
	AND,
	DOUBLE_AND,
	OR,
	NOT,
	ADD_ASSIGN,
	SUB_ASSIGN,
	MUL_ASSIGN,
	DIV_ASSIGN,
	DIVF_ASSIGN,

	// Punctuation / Delimiters
	LPAREN,
	RPAREN,
	LBRACKET,
	RBRACKET,
	LBRACE,
	RBRACE,
	COMMA,
	SEMI,
	DOT,
	COLON,
	COLON_COLON,

	// Special
	RETURN_TYPE,
	NEW,
	END_OF_FILE,
	COMMENT,
	UNKNOWN,

	NUM_TOKENS
};

/**
 * @class Token
 * @brief Represents a single source code token produced by the lexer.
 *
 * Stores the token type, raw string slice, and location metadata
 * (line and column numbers) useful for debugging and error messaging.
 */
class Token {
	private:
		TokenKind kind;  ///< The category of the token.
		std::string word; ///< The raw lexeme string.
		int line;         ///< The 1-based line number in source code.
		int column;       ///< The 1-based column number in source code.

	public:
		/**
		 * @brief Constructs a new Token instance.
		 * @param k The category of the token.
		 * @param w The raw string representation.
		 * @param l Source code line number.
		 * @param c Source code column number.
		 */
		Token(TokenKind k, std::string_view w, int l, int c)
			: kind(k), word(w), line(l), column(c) {}

		/// @brief Gets the category of the token.
		TokenKind getKind() const { return this->kind; }

		/// @brief Gets the raw string value of the token.
		std::string getWord() const { return this->word; }

		/// @brief Gets the source code line number.
		int getLine() const { return this->line; }

		/// @brief Gets the source code column number.
		int getColumn() const { return this->column; }

		/// @brief Returns a human-readable string representation of the token kind.
		std::string toString();
};

#endif // TOKEN_TYPE_HPP
