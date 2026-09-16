#include "Lexer.hpp"
#include <cstdint>
#include <emmintrin.h>
#include <iostream>
#include <vector>

/*------------*/
/* DFA states */
/*------------*/

enum class State : uint8_t {
    S0,    // start
    S1,    // after '+'
    S2,    // after '-'
    S3,    // after '/'
    S4,    // standard decimal integer digits
    S5,    // digit(s) then '.', expects at least one more digit
    S6,    // float digits after '.'
    S7,    // identifier / keyword
    S8,    // after '='
    S9,    // after '<'
    S10,   // after '>'
    S11,   // after '&'
    S12,   // after '!'
    S13,   // after '|'
    S14,   // inside double-quoted string
    S15,   // single-quote open
    S16,   // single char body inside single-quotes
    S17,   // backslash escape inside single-quotes
    S18,   // character after escape sequence, awaiting closing quote
    S19,   // after '*'

    // Prefixed Literal States
    S4_ZERO,       // saw leading '0'
    S4_BIN_PREFIX, // saw '0b' or '0B', expecting binary digits
    S4_BIN,        // consuming binary digits
    S4_OCT_PREFIX, // saw '0o' or '0O', expecting octal digits
    S4_OCT,        // consuming octal digits
    S4_HEX_PREFIX, // saw '0x' or '0X', expecting hex digits
    S4_HEX,        // consuming hex digits

    // Multi-character operator terminal states
    S1_PLUS_PLUS,   // "++"
    S2_MINUS_MINUS, // "--"
    S2_ARROW,       // "->"
    S8_FAT_ARROW,   // "=>"
    S9_LE,          // "<="
    S10_GE,         // ">="
    S11_BORROW,     // "&="
    S11_DOUBLE_AND, // "&&"
    S12_NE,         // "!="
    S8_EQ,          // "=="
    S13_OR,         // "||"
    S19_POWER,      // "**"

    END,
    ERR
};


/*------------------*/
/* Input categories */
/*------------------*/

enum class InputCat : uint8_t {
	PLUS,         // 0
	MINUS,        // 1
	SLASH,        // 2
	DIGIT_01,     // 3  '0', '1'
	DIGIT_27,     // 4  '2'-'7'
	DIGIT_89,     // 5  '8', '9'
	DOT,          // 6  '.'
	PREFIX_B,     // 7  'b', 'B'
	PREFIX_O,     // 8  'o', 'O'
	PREFIX_X,     // 9  'x', 'X'
	HEX_LETTER,   // 10 'a'-'f', 'A'-'F' (excluding b, o, x)
	LETTER_UNDER, // 11 Other letters / '_'
	EQUAL,        // 12 '='
	LT,           // 13 '<'
	GT,           // 14 '>'
	AMP,          // 15 '&'
	BANG,         // 16 '!'
	PIPE,         // 17 '|'
	QUOTE_D,      // 18 '"'
	QUOTE_S,      // 19 '\''
	BRACKET,      // 20
	UNI_CHAR,     // 21
	BACKSLASH,    // 22
	PROD,         // 23
	OTHER,        // 24
	COUNT
};

static constexpr int NSTATES = 39;
static constexpr int NCATS = static_cast<int>(InputCat::COUNT); // 25

static constexpr State E = State::END;
static constexpr State ER = State::ERR;

/*-------------------------------------------------------------------------------------------------*/
/* Transition table  T[state][inputCat]                                                            */
/* Column order:                                                                                   */
/* +  -  / 01 27 89  .  b  o  x hex_let let_=  <  >  &  !  |  "  ' [] ~ \ ?                         */
/*-------------------------------------------------------------------------------------------------*/

static constexpr State T[NSTATES][NCATS] = {
    /*S0 */ {State::S1, State::S2, State::S3, State::S4_ZERO, State::S4, State::S4, E, State::S7, State::S7, State::S7, State::S7, State::S7, State::S8, State::S9, State::S10, State::S11, State::S12, State::S13, State::S14, State::S15, E, E, E, State::S19, ER},
    /*S1 */ {State::S1_PLUS_PLUS, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S2 */ {E, State::S2_MINUS_MINUS, E, E, E, E, E, E, E, E, E, E, E, State::S2_ARROW, E, E, E, E, E, E, E, E, E, E, E},
    /*S3 */ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S4 */ {E, E, E, State::S4, State::S4, State::S4, State::S5, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S5 */ {ER, ER, ER, State::S6, State::S6, State::S6, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER},
    /*S6 */ {E, E, E, State::S6, State::S6, State::S6, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S7 */ {E, E, E, State::S7, State::S7, State::S7, E, State::S7, State::S7, State::S7, State::S7, State::S7, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S8 */ {E, E, E, E, E, E, E, E, E, E, E, E, State::S8_EQ, E, State::S8_FAT_ARROW, E, E, E, E, E, E, E, E, E, E},
    /*S9 */ {E, E, E, E, E, E, E, E, E, E, E, E, State::S9_LE, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S10*/ {E, E, E, E, E, E, E, E, E, E, E, E, State::S10_GE, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S11*/ {E, E, E, E, E, E, E, E, E, E, E, E, State::S11_BORROW, E, E, State::S11_DOUBLE_AND, E, E, E, E, E, E, E, E, E},
    /*S12*/ {E, E, E, E, E, E, E, E, E, E, E, E, State::S12_NE, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S13*/ {ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, State::S13_OR, ER, ER, ER, ER, ER, E, ER},
    /*S14*/ {State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14, E, State::S14, State::S14, State::S14, State::S14, State::S14, State::S14},
    /*S15*/ {State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, State::S16, ER, State::S16, State::S16, State::S17, State::S16, State::S16},
    /*S16*/ {ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, E, ER, ER, ER, ER, ER},
    /*S17*/ {State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18, State::S18},
    /*S18*/ {ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, E, ER, ER, ER, ER},
    /*S19*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, State::S19_POWER, E}, // After '*'
    /*S4_ZERO*/ {E, E, E, State::S4, State::S4, State::S4, State::S5, State::S4_BIN_PREFIX, State::S4_OCT_PREFIX, State::S4_HEX_PREFIX, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S4_BIN_PREFIX*/ {ER, ER, ER, State::S4_BIN, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER},
    /*S4_BIN*/ {E, E, E, State::S4_BIN, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S4_OCT_PREFIX*/ {ER, ER, ER, State::S4_OCT, State::S4_OCT, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER},
    /*S4_OCT*/ {E, E, E, State::S4_OCT, State::S4_OCT, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S4_HEX_PREFIX*/ {ER, ER, ER, State::S4_HEX, State::S4_HEX, State::S4_HEX, ER, State::S4_HEX, ER, ER, State::S4_HEX, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER, ER},
    /*S4_HEX*/ {E, E, E, State::S4_HEX, State::S4_HEX, State::S4_HEX, E, State::S4_HEX, E, E, State::S4_HEX, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S1_PLUS_PLUS*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S2_MINUS_MINUS*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S2_ARROW*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S8_FAT_ARROW*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S9_LE*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S10_GE*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S11_BORROW*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S11_DOUBLE_AND*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S12_NE*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S8_EQ*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S13_OR*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
    /*S19_POWER*/ {E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E, E},
};

/*------------------------------*/
/* State -> TokenKind flat map  */
/*------------------------------*/

static constexpr TokenKind StateToToken[] = {
    /* S0             */ TokenKind::UNKNOWN,
    /* S1             */ TokenKind::ADD,
    /* S2             */ TokenKind::SUB,
    /* S3             */ TokenKind::DIV,
    /* S4             */ TokenKind::LIT_INT,
    /* S5             */ TokenKind::UNKNOWN,
    /* S6             */ TokenKind::LIT_FLOAT,
    /* S7             */ TokenKind::IDENTIFIER,
    /* S8             */ TokenKind::ASSIGN,
    /* S9             */ TokenKind::LT,
    /* S10            */ TokenKind::GT,
    /* S11            */ TokenKind::AND,
    /* S12            */ TokenKind::NOT,
    /* S13            */ TokenKind::UNKNOWN,
    /* S14            */ TokenKind::LIT_STRING,
    /* S15            */ TokenKind::LIT_CHAR,
    /* S16            */ TokenKind::LIT_CHAR,
    /* S17            */ TokenKind::LIT_CHAR,
    /* S18            */ TokenKind::LIT_CHAR,
    /* S19            */ TokenKind::PROD,
    /* S4_ZERO        */ TokenKind::LIT_INT,
    /* S4_BIN_PREFIX  */ TokenKind::UNKNOWN,
    /* S4_BIN         */ TokenKind::LIT_INT,
    /* S4_OCT_PREFIX  */ TokenKind::UNKNOWN,
    /* S4_OCT         */ TokenKind::LIT_INT,
    /* S4_HEX_PREFIX  */ TokenKind::UNKNOWN,
    /* S4_HEX         */ TokenKind::LIT_INT,
    /* S1_PLUS_PLUS   */ TokenKind::INCREMENT,
    /* S2_MINUS_MINUS */ TokenKind::DECREMENT,
    /* S2_ARROW       */ TokenKind::RETURN_TYPE,
    /* S8_FAT_ARROW   */ TokenKind::FAT_ARROW,
    /* S9_LE          */ TokenKind::LE,
    /* S10_GE         */ TokenKind::GE,
    /* S11_BORROW     */ TokenKind::BORROW,
    /* S11_DOUBLE_AND */ TokenKind::DOUBLE_AND,
    /* S12_NE         */ TokenKind::NE,
    /* S8_EQ          */ TokenKind::EQ,
    /* S13_OR         */ TokenKind::OR,
    /* S19_POWER      */ TokenKind::POWER
};

/*------------------------------------------------------------------*/
/* ASCII character -> InputCat lookup (128 entries)                 */
/*------------------------------------------------------------------*/

static constexpr InputCat catTable[128] = {
	// 0x00-0x1F control characters
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,
	InputCat::OTHER, InputCat::OTHER, InputCat::OTHER, InputCat::OTHER,

	// 0x20-0x2F punctuation / operators
	InputCat::OTHER,     // 0x20 ' '
	InputCat::BANG,      // 0x21 '!'
	InputCat::QUOTE_D,   // 0x22 '"'
	InputCat::OTHER,     // 0x23 '#'
	InputCat::OTHER,     // 0x24 '$'
	InputCat::UNI_CHAR,  // 0x25 '%'
	InputCat::AMP,       // 0x26 '&'
	InputCat::QUOTE_S,   // 0x27 '\''
	InputCat::BRACKET,   // 0x28 '('
	InputCat::BRACKET,   // 0x29 ')'
	InputCat::PROD,  // 0x2A '*'
	InputCat::PLUS,      // 0x2B '+'
	InputCat::UNI_CHAR,  // 0x2C ','
	InputCat::MINUS,     // 0x2D '-'
	InputCat::DOT,       // 0x2E '.'
	InputCat::SLASH,     // 0x2F '/'

	// 0x30-0x39 '0'-'9'
	InputCat::DIGIT_01, InputCat::DIGIT_01, // '0', '1'
	InputCat::DIGIT_27, InputCat::DIGIT_27, InputCat::DIGIT_27, InputCat::DIGIT_27, InputCat::DIGIT_27, InputCat::DIGIT_27, // '2'-'7'
	InputCat::DIGIT_89, InputCat::DIGIT_89, // '8', '9'

	// 0x3A-0x40
	InputCat::OTHER,     // 0x3A ':'
	InputCat::UNI_CHAR,  // 0x3B ';'
	InputCat::LT,        // 0x3C '<'
	InputCat::EQUAL,     // 0x3D '='
	InputCat::GT,        // 0x3E '>'
	InputCat::OTHER,     // 0x3F '?'
	InputCat::OTHER,     // 0x40 '@'

	// 0x41-0x5A 'A'-'Z'
	InputCat::HEX_LETTER,   // 'A'
	InputCat::PREFIX_B,     // 'B'
	InputCat::HEX_LETTER,   // 'C'
	InputCat::HEX_LETTER,   // 'D'
	InputCat::HEX_LETTER,   // 'E'
	InputCat::HEX_LETTER,   // 'F'
	InputCat::LETTER_UNDER, // 'G'
	InputCat::LETTER_UNDER, // 'H'
	InputCat::LETTER_UNDER, // 'I'
	InputCat::LETTER_UNDER, // 'J'
	InputCat::LETTER_UNDER, // 'K'
	InputCat::LETTER_UNDER, // 'L'
	InputCat::LETTER_UNDER, // 'M'
	InputCat::LETTER_UNDER, // 'N'
	InputCat::PREFIX_O,     // 'O'
	InputCat::LETTER_UNDER, // 'P'
	InputCat::LETTER_UNDER, // 'Q'
	InputCat::LETTER_UNDER, // 'R'
	InputCat::LETTER_UNDER, // 'S'
	InputCat::LETTER_UNDER, // 'T'
	InputCat::LETTER_UNDER, // 'U'
	InputCat::LETTER_UNDER, // 'V'
	InputCat::LETTER_UNDER, // 'W'
	InputCat::PREFIX_X,     // 'X'
	InputCat::LETTER_UNDER, // 'Y'
	InputCat::LETTER_UNDER, // 'Z'

	// 0x5B-0x60
	InputCat::BRACKET,   // 0x5B '['
	InputCat::BACKSLASH, // 0x5C '\'
	InputCat::BRACKET,   // 0x5D ']'
	InputCat::OTHER,     // 0x5E '^'
	InputCat::LETTER_UNDER, // 0x5F '_'
	InputCat::OTHER,     // 0x60 '`'

	// 0x61-0x7A 'a'-'z'
	InputCat::HEX_LETTER,   // 'a'
	InputCat::PREFIX_B,     // 'b'
	InputCat::HEX_LETTER,   // 'c'
	InputCat::HEX_LETTER,   // 'd'
	InputCat::HEX_LETTER,   // 'e'
	InputCat::HEX_LETTER,   // 'f'
	InputCat::LETTER_UNDER, // 'g'
	InputCat::LETTER_UNDER, // 'h'
	InputCat::LETTER_UNDER, // 'i'
	InputCat::LETTER_UNDER, // 'j'
	InputCat::LETTER_UNDER, // 'k'
	InputCat::LETTER_UNDER, // 'l'
	InputCat::LETTER_UNDER, // 'm'
	InputCat::LETTER_UNDER, // 'n'
	InputCat::PREFIX_O,     // 'o'
	InputCat::LETTER_UNDER, // 'p'
	InputCat::LETTER_UNDER, // 'q'
	InputCat::LETTER_UNDER, // 'r'
	InputCat::LETTER_UNDER, // 's'
	InputCat::LETTER_UNDER, // 't'
	InputCat::LETTER_UNDER, // 'u'
	InputCat::LETTER_UNDER, // 'v'
	InputCat::LETTER_UNDER, // 'w'
	InputCat::PREFIX_X,     // 'x'
	InputCat::LETTER_UNDER, // 'y'
	InputCat::LETTER_UNDER, // 'z'

	// 0x7B-0x7F
	InputCat::BRACKET,   // 0x7B '{'
	InputCat::PIPE,      // 0x7C '|'
	InputCat::BRACKET,   // 0x7D '}'
	InputCat::OTHER,     // 0x7E '~'
	InputCat::OTHER,     // 0x7F DEL
};

static inline InputCat classify(char c) {
	const auto uc = static_cast<unsigned char>(c);
	return uc < 128 ? catTable[uc] : InputCat::OTHER;
}

/*-----------------*/
/* Keyword matcher */
/*-----------------*/

static inline TokenKind keywordOrIdent(std::string_view w) {
	switch (w.size()) {
		case 2:
			if (w == "if")
				return TokenKind::IF;
			if (w == "fn")
				return TokenKind::FN;
			if (w == "as")
				return TokenKind::AS;
			break;
		case 3:
			if (w == "new")
				return TokenKind::NEW;
			if (w == "for")
				return TokenKind::FOR;
			if (w == "mut")
				return TokenKind::MUT;
			break;
		case 4:
			if (w == "else")
				return TokenKind::ELSE;
			if (w == "true")
				return TokenKind::LIT_BOOL;
			if (w == "loop")
				return TokenKind::LOOP;
			if (w == "enum")
				return TokenKind::ENUM;
			break;
		case 5:
			if (w == "while")
				return TokenKind::WHILE;
			if (w == "false")
				return TokenKind::LIT_BOOL;
			if (w == "const")
				return TokenKind::CONST;
			if (w == "break")
				return TokenKind::BREAK;
			if (w == "match")
				return TokenKind::MATCH;
			break;
		case 6:
			if (w == "return")
				return TokenKind::RETURN;
			if (w == "import")
				return TokenKind::IMPORT;
			if (w == "public")
				return TokenKind::PUBLIC;
			break;
		case 7:
			if (w == "private")
				return TokenKind::PRIVATE;
			break;
		case 8:
			if (w == "continue")
				return TokenKind::CONTINUE;
			break;
		case 9:
			if (w == "protected")
				return TokenKind::PRIVATE;
			break;
	}
	return TokenKind::IDENTIFIER;
}

/*-------------------*/
/* Single-char kinds */
/*-------------------*/

static inline TokenKind singleCharKind(char c) {
	switch (c) {
		case '(':
			return TokenKind::LPAREN;
		case ')':
			return TokenKind::RPAREN;
		case '[':
			return TokenKind::LBRACKET;
		case ']':
			return TokenKind::RBRACKET;
		case '{':
			return TokenKind::LBRACE;
		case '}':
			return TokenKind::RBRACE;
		case ';':
			return TokenKind::SEMI;
		case ',':
			return TokenKind::COMMA;
		case '%':
			return TokenKind::MOD;
		case '.':
			return TokenKind::DOT;
		default:
			return TokenKind::UNKNOWN;
	}
}

/*------------------*/
/* Comment skippers */
/*------------------*/

static inline size_t skipLineComment(const char *src, size_t pos, size_t srcLen,
		size_t &col) {
	while (pos < srcLen && src[pos] != '\n') {
		++pos;
		++col;
	}
	return pos;
}

static inline size_t skipBlockComment(const char *src, size_t pos,
		size_t srcLen, char closeA, char closeB,
		size_t &line, size_t &col) {
	while (pos + 1 < srcLen) {
		if (src[pos] == closeA && src[pos + 1] == closeB) {
			pos += 2;
			col += 2;
			return pos;
		}
		if (src[pos] == '\n') {
			++line;
			col = 0;
		}
		++col;
		++pos;
	}
	std::cerr << "\033[31mUnterminated block comment\033[0m\n";
	return srcLen;
}

/*--------------------*/
/* Whitespace skipper */
/*--------------------*/

void Lexer::skipWhitespace() {
	const char *p = src + pos;
	const char *end = src + srcLen;

	// Scalar prefix — handles newlines and brings p to a natural alignment
	// for the SIMD block (or exits early if already at non-whitespace).
	auto scalarSkip = [&]() {
		while (p < end && static_cast<unsigned char>(*p) <= 0x20) {
			if (*p == '\n') {
				++line;
				col = 0;
			}
			++col;
			++p;
		}
	};

	scalarSkip();

	// SSE2: skip 16 bytes at a time when the entire chunk is whitespace.
	const __m128i thresh = _mm_set1_epi8(0x20);
	while (p + 16 <= end) {
		__m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
		__m128i cmp = _mm_cmpgt_epi8(chunk, thresh);
		int mask = _mm_movemask_epi8(cmp);

		if (mask != 0) {
			// At least one non-whitespace byte in this chunk.
			const int skip = __builtin_ctz(mask); // leading whitespace bytes
			for (int i = 0; i < skip; ++i) {
				if (p[i] == '\n') {
					++line;
					col = 0;
				}
				++col;
			}
			p += skip;
			break;
		}

		// Whole 16-byte chunk is whitespace — track newlines and advance.
		for (int i = 0; i < 16; ++i) {
			if (p[i] == '\n') {
				++line;
				col = 0;
			}
			++col;
		}
		p += 16;
	}

	// Scalar tail — remainder after the last full 16-byte chunk.
	scalarSkip();
	pos = static_cast<size_t>(p - src);
}

/*--------------*/
/* Token helper */
/*--------------*/

Token Lexer::makeToken(TokenKind k, std::string_view spelling) const {
	return Token(k, spelling, line, col);
}

/*------------------------*/
/* Main tokenisation loop */
/*------------------------*/

std::vector<Token> Lexer::Tokenize() {
	std::vector<Token> tokens;
	tokens.reserve(srcLen > 16 ? (srcLen / 4) + 8 : 8);

	while (pos < srcLen) {
		skipWhitespace();
		if (pos >= srcLen)
			break;

		char c = src[pos];

		// Null byte → EOF marker (shouldn't appear in well-formed source).
		if (c == '\0') {
			++pos;
			tokens.push_back(makeToken(TokenKind::END_OF_FILE, "<EOF>"));
			continue;
		}

		// Comments — must be checked before the DFA so '//' isn't seen as two DIVs.
		if (c == '/' && pos + 1 < srcLen) {
			const char next = src[pos + 1];
			if (next == '/') {
				pos += 2;
				col += 2;
				pos = skipLineComment(src, pos, srcLen, col);
				continue;
			}
			if (next == '*') {
				pos += 2;
				col += 2;
				pos = skipBlockComment(src, pos, srcLen, '*', '/', line, col);
				continue;
			}
			if (next == '!') {
				pos += 2;
				col += 2;
				pos = skipBlockComment(src, pos, srcLen, '!', '/', line, col);
				continue;
			}
		}

		// '::' and ':'
		if (c == ':') {
			if (pos + 1 < srcLen && src[pos + 1] == ':') {
				tokens.push_back(makeToken(TokenKind::COLON_COLON, "::"));
				pos += 2;
				col += 2;
			} else {
				tokens.push_back(makeToken(TokenKind::COLON, ":"));
				++pos;
				++col;
			}
			continue;
		}

		// Compound assignment operators: +=  -=  *=  /=
		// Checked before the DFA so they don't get split into two tokens.
		if (pos + 1 < srcLen) {
			const char next = src[pos + 1];

			// Flèche de type '->'
			if (c == '-' && next == '>') {
				tokens.push_back(makeToken(TokenKind::RETURN_TYPE, "->"));
				pos += 2;
				col += 2;
				continue;
			}

			// Fat arrow '=>'
			if (c == '=' && next == '>') {
				tokens.push_back(makeToken(TokenKind::FAT_ARROW, "=>"));
				pos += 2;
				col += 2;
				continue;
			}

			// Opérateurs d'affectation : +=  -=  *=  /=
			if (next == '=') {
				TokenKind kind = TokenKind::UNKNOWN;
				switch (c) {
					case '+': kind = TokenKind::ADD_ASSIGN; break;
					case '-': kind = TokenKind::SUB_ASSIGN; break;
					case '*': kind = TokenKind::MUL_ASSIGN; break;
					case '/': kind = TokenKind::DIV_ASSIGN; break;
					default: break;
				}
				if (kind != TokenKind::UNKNOWN) {
					tokens.push_back(makeToken(kind, std::string_view(src + pos, 2)));
					pos += 2;
					col += 2;
					continue;
				}
			}
		}
		/*------------------------*/
		/* DFA-based tokenisation */
		/*------------------------*/

		const int icat = static_cast<int>(classify(c));
		const State first = T[0][icat];

		if (first == State::END) {
			tokens.push_back(
					makeToken(singleCharKind(c), std::string_view(src + pos, 1)));
			++pos;
			++col;
			continue;
		}

		if (first == State::ERR) {
			std::cerr << "\033[31mUnknown symbol [" << c << "]\033[0m\n";
			tokens.push_back(makeToken(TokenKind::UNKNOWN, "<UNKNOWN>"));
			++pos;
			++col;
			continue;
		}

		const size_t spellingStart = pos;
		const bool isLiteral = (first == State::S14 || first == State::S15);

		++pos;
		++col;

		State state = first;
		State lastSignificantState = first;

		while (pos < srcLen) {
			c = src[pos];
			const int icat2 = static_cast<int>(classify(c));
			const State ns = T[static_cast<int>(state)][icat2];

			if (ns == State::ERR) {
				state = ns;
				break;
			}

			// Multi-char terminal operator: consume the final character and stop.
			if (ns >= State::S1_PLUS_PLUS && ns <= State::S13_OR) {
				lastSignificantState = ns;
				++pos;
				++col;
				state = State::END;
				break;
			}

			if (state != State::END && state != State::ERR)
				lastSignificantState = state;

			if (ns == State::END) {
				// Consume the closing quote for string/char literals.
				if ((state == State::S14 &&
							static_cast<InputCat>(icat2) == InputCat::QUOTE_D) ||
						((state == State::S16 || state == State::S18) &&
						 static_cast<InputCat>(icat2) == InputCat::QUOTE_S)) {
					++pos;
					++col;
					lastSignificantState = state;
				}
				state = ns;
				break;
			}

			if (c == '\n') {
				++line;
				col = 0;
			}
			++col;
			++pos;
			state = ns;
		}

		if (state == State::ERR) {
			std::cerr << "\033[31mLexer error near ["
				<< std::string_view(src + spellingStart, pos - spellingStart)
				<< "]\033[0m\n";
			tokens.push_back(makeToken(TokenKind::UNKNOWN, "<UNKNOWN>"));
			continue;
		}

		const size_t spellingLen = pos - spellingStart;
		// Strip enclosing quotes from string/char literals so the spelling
		// contains only the content (e.g. hello from "hello", n from '\n').
		std::string_view spelling =
			isLiteral ? std::string_view(src + spellingStart + 1,
					spellingLen > 2 ? spellingLen - 2 : 0)
			: std::string_view(src + spellingStart, spellingLen);

		const TokenKind finalKind =
			(lastSignificantState == State::S7)
			? keywordOrIdent(spelling)
			: StateToToken[static_cast<size_t>(lastSignificantState)];

		tokens.push_back(makeToken(finalKind, spelling));
	}

	return tokens;
}
