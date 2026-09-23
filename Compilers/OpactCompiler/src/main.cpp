/**
 * @file main.cpp
 * @brief Starting script and main manager for compiler.
 */

#include "Parser/CompilerPipeline.hpp"

// External packages
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// -------------------- //
// Windows & unix check //
// -------------------- //

/**
 * @brief get's the correct home directory depending on the OS
 *
 * The actual function uses a check for win32 and returns the path of the home env
 * if it's linux we just get the home path directly with getenv
 * 
 * @return string :the path of home directory 
 * */
std::string getHomeDirectory() {
	const char *home = nullptr;

#ifdef _WIN32
	home = std::getenv("USERPROFILE");
	if (!home) {
		const char *homedrive = std::getenv("HOMEDRIVE");
		const char *homepath = std::getenv("HOMEPATH");
		if (homedrive && homepath) {
			return std::string(homedrive) + homepath;
		}
	}
#else
	home = std::getenv("HOME");
#endif

	return home ? home : "";
}

/**
 * @brief get's the config directory
 *
 * based on your os it gives you the correct path for your Operating system .config %appdata% 
 * 
 * @return fs::path : returns the path of config
 * */
fs::path getConfigDir() {
	std::string home = getHomeDirectory();
	if (home.empty()) {
		return fs::current_path() / "opact_config";
	}

#ifdef _WIN32
	// Windows: %APPDATA%\opact
	const char *appdata = std::getenv("APPDATA");
	if (appdata) {
		return fs::path(appdata) / "opact";
	}
	return fs::path(home) / "AppData" / "Roaming" / "opact";
#else
	// Linux/macOS: ~/.config/opact
	return fs::path(home) / ".config" / "opact";
#endif
}

// ------------------ //
// Utility functions  //
// ------------------ //

/**
 * @brief determines if the file ends with a given extension
 * @return bool : if it ends or not with the extension
 * */
bool endsWith(const std::string &str, const std::string &suffix) {
	if (str.length() < suffix.length())
		return false;
	return str.compare(str.length() - suffix.length(), suffix.length(), suffix) ==
		0;
}

/**
 * @brief Detrermines if the extension is part of the valid ones I chose
 * @return bool : if it ends with .op
 * */
bool hasValidExt(const std::string &f) { return endsWith(f, ".op"); }

std::string getOutputName(const std::string &file) {
	size_t pos = file.find_last_of('.');
	std::string base = (pos == std::string::npos) ? file : file.substr(0, pos);

#ifdef _WIN32
	return base + ".exe";
#else
	return base + ".x";
#endif
}

// ------------------ //
// Main function	  //
// ------------------ //
//
/**
 * @brief Start of the compiler.
 *
 * This allows to do different commands and modes for the compiler. Example : version, project file.
 * This also does a check if the file is valid correct extension and exist.
 * 
 * @param argc number of arguments.
 * @param argv list of arguments
 * @return success or not of the compiler
 */
int main(int argc, char *argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: opact [options] [files...]\n";
		std::cerr << "Options:\n";
		std::cerr << "	--version	   Show version information\n";
		return EXIT_FAILURE;
	}

	std::string firstArg = argv[1];

	if (firstArg == "--version") {
		std::cout << "opact "
		             "[2026.09.18]\n";
		return EXIT_SUCCESS;
	}

	std::vector<std::string> inputs;
	for (int i = 1; i < argc; i++) {
		if (hasValidExt(argv[i])) {
			inputs.push_back(argv[i]);
		}
	}

	if (inputs.empty()) {
		std::cerr << "Error: No valid '.op' source files provided.\n";
		return EXIT_FAILURE;
	}

	auto start = std::chrono::high_resolution_clock::now();

	// Initialize the thread-safe compiler orchestrator
	CompilerPipeline pipeline;

	// Enqueue initial entry files supplied via command-line arguments
	for (const std::string &file : inputs) {
		std::cout << "Starting compilation entry point: " << file << "\n";
		pipeline.enqueueFile(file);
	}

	pipeline.runPipeline();

	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> time = end - start; 
	double timeInMs = time.count();
	double tokensPerSec = (static_cast<double>(pipeline.nbtokens) / timeInMs) * 1000.0;

	std::cout << "Time	   : " << timeInMs << " ms\n";
	std::cout << "Tokens/s : " << tokensPerSec << " tokens/s\n";
	std::cout << "Multi-threaded parsing phase complete.\n";

	return EXIT_SUCCESS;
}
