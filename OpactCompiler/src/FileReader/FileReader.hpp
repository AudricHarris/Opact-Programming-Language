/**
 * @file FileReader.hpp
 * @brief this Handles reading a file this used to have a bigger role. Now it only reads a file at given name
 * */
#ifndef File_Reader
#define File_Reader

#include <optional>
#include <string>

/** @brief reads the file that you put as in params and returns a string if it exists*/
std::optional<std::string> readFile(const char *name);

#endif
