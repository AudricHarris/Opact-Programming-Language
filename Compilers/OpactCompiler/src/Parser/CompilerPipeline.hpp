#ifndef COMPILER_PIPELINE_HPP
#define COMPILER_PIPELINE_HPP

#include "Ast.hpp"
#include "FileReader/FileReader.hpp"
#include "Lexer/Lexer.hpp"
#include "Parser/Parser.hpp"
#include "TypeChecker/TypeChecker.hpp"
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <condition_variable>

namespace fs = std::filesystem;

struct Module {
	std::string filePath;
	ExprPtr astRoot;
};

class CompilerPipeline {
	private:
		std::mutex queueMutex;
		std::condition_variable cv;
		std::queue<std::string> workQueue;
		std::unordered_set<std::string> visitedFiles;
		int activeWorkers = 0;

		std::mutex astMutex;
		std::unordered_map<std::string, Module> parsedModules;

	public:
		std::atomic<int> nbtokens{0};

		// Accepts imported path and optional parent file path to resolve relative imports
		void enqueueFile(const std::string& path, const std::string& parentPath = "") {
			fs::path targetPath(path);

			// Resolve path relative to the directory of the parent file
			if (!parentPath.empty() && targetPath.is_relative()) {
				fs::path parentDir = fs::path(parentPath).parent_path();
				targetPath = (parentDir / targetPath).lexically_normal();
			} else {
				targetPath = targetPath.lexically_normal();
			}

			std::string canonicalString = targetPath.string();

			{
				std::lock_guard<std::mutex> lock(this->queueMutex);
				if (this->visitedFiles.find(canonicalString) == this->visitedFiles.end()) {
					this->visitedFiles.insert(canonicalString);
					this->workQueue.push(canonicalString);
					this->cv.notify_one();
				} else {
					std::cerr << "\033[31m\033[1m[Import Error]\033[0m\033[31m Tried to import already imported file '" 
						<< canonicalString << "': circular import detected or already queued.\033[0m\n";
				}
			}
		}

		void runPipeline() {
			unsigned int threadCount = std::thread::hardware_concurrency();
			if (threadCount == 0) threadCount = 2;

			std::vector<std::thread> workers;
			for (unsigned int i = 0; i < threadCount; ++i) {
				workers.emplace_back(&CompilerPipeline::workerLoop, this);
			}

			for (auto& t : workers) {
				if (t.joinable()) t.join();
			}
		}

	private:
		void workerLoop() {
			while (true) {
				std::string fileToParse;

				{
					std::unique_lock<std::mutex> lock(this->queueMutex);

					// Wait until there is work available OR all workers are idle (queue drained)
					this->cv.wait(lock, [this]() {
							return !this->workQueue.empty() || this->activeWorkers == 0;
							});

					if (this->workQueue.empty() && this->activeWorkers == 0) {
						this->cv.notify_all(); // Wake up any remaining threads so they can exit
						return;
					}

					fileToParse = this->workQueue.front();
					this->workQueue.pop();
					this->activeWorkers++;
				}

				Module module = this->parseSingleFile(fileToParse);

				{
					std::lock_guard<std::mutex> lock(this->astMutex);
					this->parsedModules[fileToParse] = std::move(module);
				}

				{
					std::lock_guard<std::mutex> lock(this->queueMutex);
					this->activeWorkers--;
					this->cv.notify_all();
				}
			}
		}

		Module parseSingleFile(const std::string& path) {
			std::optional<std::string> content = readFile(path.c_str());
			if (!content.has_value()) {
				std::cerr << "File : " << path << " was not found\n";
				return Module{ path, nullptr }; 
			}

			const std::string &code = content.value();

			Lexer l(code);
			std::vector<Token> codeTokenized = l.Tokenize();
			for (Token t : codeTokenized)
				std::cout << t.toString();

			this->nbtokens += static_cast<int>(codeTokenized.size()); 

			Parser p(std::move(codeTokenized), path, this);
			ExprPtr file = p.parseModule();
			
			TypeChecker t;
			t.initialize(file);

			return Module{ path, std::move(file) }; 
		}
};

#endif // COMPILER_PIPELINE_HPP
