#include "FileReader.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

std::string fileToString(const std::string &filename) {
	std::ifstream file(filename, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>());
}

std::optional<std::string> readFile(const char *name) {
	std::string filename = name;

	try {

		std::cout << "File : " << filename << "\n";
		std::string content = fileToString(filename);
		return content;

	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << "\n";
		return std::nullopt;
	}
}
