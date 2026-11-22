#ifndef CODEGEN_STRINGUTIL_HPP
#define CODEGEN_STRINGUTIL_HPP

//==========================================================================//
//  StringUtil.hpp  -  small string helpers for literal handling            //
//==========================================================================//

#include <cstddef>
#include <string>

namespace codegen_detail {

// Removes one pair of surrounding quotes if the lexer kept them.
inline std::string stripQuotes(const std::string& s) {
	if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
	                      (s.front() == '\'' && s.back() == '\''))) {
		return s.substr(1, s.size() - 2);
	}
	return s;
}

// Turns the two characters '\' 'n' into a real newline, etc.
// (harmless if the lexer already did it).
inline std::string unescape(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '\\' && i + 1 < s.size()) {
			switch (s[++i]) {
				case 'n':  out += '\n'; break;
				case 't':  out += '\t'; break;
				case 'r':  out += '\r'; break;
				case '0':  out += '\0'; break;
				case '\\': out += '\\'; break;
				case '"':  out += '"';  break;
				case '\'': out += '\''; break;
				default:   out += '\\'; out += s[i]; break;
			}
		} else {
			out += s[i];
		}
	}
	return out;
}

inline std::string trim(const std::string& s) {
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

} // namespace codegen_detail

// Expands \e, \xHH and \033-style escapes into raw bytes (implemented in
// StringUtil.cpp). Run it BEFORE codegen_detail::unescape().
std::string expandAnsiEscapes(const std::string& s);

#endif // CODEGEN_STRINGUTIL_HPP
