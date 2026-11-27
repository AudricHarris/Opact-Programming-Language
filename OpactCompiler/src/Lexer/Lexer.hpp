/**
 * @file Lexer.hpp
 * @brief This is the header file for the lexer which is a class that transform source code into tokens
 */
#ifndef LEXER_H
#define LEXER_H

#include "../Token/TokenType.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/**
 * @class Parser
 * @brief The lexer has simple functions and variables to simplify the process of tokenization
 */
class Lexer {
	public:
		/**@brief constructor for the Lexer, it takes a source file in string format*/
		explicit Lexer(const std::string &source)
			: src(source.data()), srcLen(source.size()) {}

		/**@brief Entry function so that it starts reading the string and making tokens*/
		std::vector<Token> Tokenize();

	private:
		const char *src;
		size_t srcLen;
		size_t pos = 0;
		size_t line = 1;
		size_t col = 1;

		void skipWhitespace();
		Token makeToken(TokenKind k, std::string_view spelling) const;
};

#endif // LEXER_H
