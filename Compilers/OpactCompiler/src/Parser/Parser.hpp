/**
 * @file Parser.hpp
 * @brief The header file that defines the Parser Class 
 */

#ifndef PARSER_H
#define PARSER_H 

// Custom
#include "Parser/Ast.hpp"
#include "Token/TokenType.hpp"
// Library
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class CompilerPipeline;

/**
 * @class Parser
 * @brief The parser is the file that transform a list of tokens into a ast
 * The parser is based of the very common gradient descent iteration (It was the simplest and most common so I chose that)
 * It uses CompilerPipeline for adding modules and making treating multiple files in multiple threads speeding up the process.
 *
 * With time these concept and this parser will be expanded to contain more features and fixes.
 */
class Parser {
	private:
		std::vector<Token> tokens; ///< The list of tokens that originated from the source file
		size_t idx = 0; ///< The idx of the current token.

		// Module Manager
		CompilerPipeline* pipeline = nullptr; ///< The pipeline which allows to parse modules and therefore do multithreaded parsing with each file
		std::string filePath; ///< The file name (not too used in our situation but it might be used for debugging 

		/** @brief this allows to return the token at current idx */
		const Token &peek() const;

		/** @brief this allows to return the token at the offset of the current idx */
		const Token &peekAt(size_t offset) const;
		
		/** @brief this return the token at current idx and increments the idx */
		const Token consume();

		/** @brief this return true if the current token at idx corresponds to the same type as the param token kind */
		bool match(TokenKind Kind);
		
		/** @brief this return true if the current token at idx corresponds to the same type as the param token kind, if true does this->consume()*/
		bool check(TokenKind kind) const;
		
		/** @brief this returns true if there are no more tokens */
		bool isAtEnd() const;
		
		/** @brief formats a string response for the Parser errors */
		std::string generateError(TokenKind kind, std::string errorMsg);
		
		/** @brief this expects a token if yes then returns the token else returns an error */
		Token expect(TokenKind kind, std::string_view errorMsg);
		
		/** @brief Adds the module with the CompilerPipeline if one is detected*/
		void addModule(std::vector<std::string> path);
	protected:
		/** @brief If there is an error with an expression this will allow to continue reading the file. Usually if this is called it means your code is invalid*/
		void synchronize();

	public:
		// Creation Process
		/** @brief Creates The parser with 3 params : list of tokens, name of path, the compiler pipeline*/
		Parser(std::vector<Token> tok, std::string path = "", CompilerPipeline* pipe = nullptr)
			: tokens(std::move(tok)), idx(0), filePath(std::move(path)), pipeline(pipe) {}

		std::vector<std::string> importFiles;

		// Methods
		
		/** @brief Parse Module is for the initial file this will call parse top level as long it is not at the end of a file*/
		ExprPtr parseModule();
		/** @brief Parse TopLevel is the maximum level concept example Functions, Classes, Enums, etc...*/
		ExprPtr parseTopLevel();
		/** @brief Parse Import checks the keyword import and adds a expression */
		ExprPtr parseImport();
		/** @brief Parse structure decl with attributes */
		ExprPtr parseStruct(std::optional<Token> visib);
		/** @brief Parse function decl with params and returns things */
		ExprPtr parseFunction(std::optional<Token> visib);
		/** @brief Parses block so this is {[expressions]} so just a list of expressions */
		ExprPtr parseBlock();
		/** @brief parses the expression which could be var decl, if expressions, while expression, etc... */
		ExprPtr parseExpression();
		/** @brief parses if (expression) and then block and then after that an else if/else with another block*/
		ExprPtr parseIf();
		/** @brief parses while (expression) and then block*/
		ExprPtr parseWhile();
		/** @brief parses loop (initially a while true might become a loop until) and then block*/
		ExprPtr parseLoop();
		
		/**
		 * @brief Parses postfix operators, indexing, member access, function calls, 
		 *		  and type casts in a left-to-right chain.
		 * @param expr The base expression node to wrap with postfix operations.
		 */
		ExprPtr parsePostfix(ExprPtr expr);

		/**
		 * @brief Parses callee, meaning that the ExprPtr callee results into a function call.
		 * @param callee The base expression that is a function that you call.
		 * */
		ExprPtr parseCallExpr(ExprPtr callee);

		/**@brief Parses the return keyword to create a return expr it also has an expression after*/
		ExprPtr parseReturn();

		/**@brief Check if the parsed elements is a var decl it's for parse expression that it's used*/
		bool parseIsVarDeclRef();
		/**@brief Parses a var decl either it's a borrow or assignement*/
		ExprPtr parseVarDecl();
		/**@brief Parses a borrow it can either be a mutable reference or just a classic reference*/
		ExprPtr parseBorrow();
		/**@brief Parses a assignement, so expression = expression (First expression can be var decl identifier)*/
		ExprPtr parseAssignement();
		/**@brief Parses a or expr (the priority is first compared to the and in the parser, so it will evaluate last between "or and "and")*/
		ExprPtr parseOr();
		/**@brief Parses the and expr (Expression and expression)*/
		ExprPtr parseAnd();
		/**@brief Parses the equality expr (Expression == Expression)*/
		ExprPtr parseEquality();
		/**@brief Parses a comparison expr (Expression > or >= or < or <= Expression)*/
		ExprPtr parseComparison();
		/**@brief Parses a addition or substraction so expr operator expr*/
		ExprPtr parseAdditive();
		/**@brief Parses a multiplication, division or modulo so expr operator expr*/
		ExprPtr parseMultiplicative();
		/**@brief Parses a unary so for example ! or - (Example !true == false )*/
		ExprPtr parseUnary();
		/**@brief Parses a primary ( which are usually litterals)*/
		ExprPtr parsePrimary();
};


#endif // !PARSER_H
