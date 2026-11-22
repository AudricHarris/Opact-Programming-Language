//==========================================================================//
//  StringUtil.cpp  -  see StringUtil.hpp                                   //
//==========================================================================//
#include "StringUtil.hpp"
#include <cctype>

// Expands the escapes the generic unescape() doesn't know, before it runs:
//   \e  \E        -> ESC (0x1b)
//   \xH / \xHH    -> that byte        (e.g. \x1b)
//   \0 / \033     -> octal byte       (e.g. \033)
// Everything else (including "\\") is copied through untouched.
// Also used by CodeGenerator.cpp for ordinary string/char literals.
std::string expandAnsiEscapes(const std::string& s) {
	auto hexVal = [](char ch) {
		return std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0'
			: std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10;
	};
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
		char n = s[i + 1];
		if (n == 'e' || n == 'E') {
			out += '\x1b';
			++i;
		} else if (n == 'x' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
			size_t j = i + 2;
			int v = 0, digits = 0;
			while (j < s.size() && digits < 2 && std::isxdigit(static_cast<unsigned char>(s[j]))) {
				v = v * 16 + hexVal(s[j]);
				++j; ++digits;
			}
			out += static_cast<char>(v);
			i = j - 1;
		} else if (n == '0' && i + 2 < s.size() && s[i + 2] >= '0' && s[i + 2] <= '7') {
			size_t j = i + 1;
			int v = 0, digits = 0;
			while (j < s.size() && digits < 3 && s[j] >= '0' && s[j] <= '7') {
				v = v * 8 + (s[j] - '0');
				++j; ++digits;
			}
			out += static_cast<char>(v);
			i = j - 1;
		} else {
			out += s[i];
			out += n;
			++i;
		}
	}
	return out;
}
