#ifndef COMPILER_PIPELINE_HPP
#define COMPILER_PIPELINE_HPP

#include "Ast.hpp"
#include "../FileReader/FileReader.hpp"
#include "../Lexer/Lexer.hpp"
#include "Parser.hpp"
#include "../TypeChecker/TypeChecker.hpp"
#include "../CodeGen/CodeGenerator.hpp"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
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
	std::vector<std::string> modulePath;
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

			this->runTypeCheckingPass();
		}

		// Helper for TypeChecker to look up other modules
		const Module* getModuleByPath(const std::string& canonicalPath) const {
			auto it = parsedModules.find(canonicalPath);
			if (it != parsedModules.end()) {
				return &it->second;
			}
			return nullptr;
		}

	private:
		void workerLoop() {
			while (true) {
				std::string fileToParse;
				{
					std::unique_lock<std::mutex> lock(this->queueMutex);
					this->cv.wait(lock, [this]() {
							return !this->workQueue.empty() || this->activeWorkers == 0;
							});

					if (this->workQueue.empty() && this->activeWorkers == 0) {
						this->cv.notify_all();
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
				return Module{ path, {}, nullptr }; 
			}

			const std::string &code = content.value();

			Lexer l(code);
			std::vector<Token> codeTokenized = l.Tokenize();
			this->nbtokens += static_cast<int>(codeTokenized.size()); 

			Parser p(std::move(codeTokenized), path, this);
			ExprPtr file = p.parseModule();
			
			std::vector<std::string> imports = p.importFiles;

			return Module{ path, imports, std::move(file) }; 
		}

		void runTypeCheckingPass() {
			for (auto& [path, module] : parsedModules) {
				if (!module.astRoot) continue;
				TypeChecker t(this, path);
				t.initialize(module.astRoot);

				llvm::LLVMContext llvmContext;
				CodeGenerator codegen("my_module", llvmContext);

				if (module.astRoot) {
					module.astRoot->accept(codegen);
				}

				if (!llvm::verifyModule(*codegen.getModule(), &llvm::errs())) {
					std::string outputPath = path;
					size_t dotPos = outputPath.find_last_of('.');
					if (dotPos != std::string::npos) {
						outputPath = outputPath.substr(0, dotPos);
					}
					std::string llPath = outputPath + ".ll";
					std::string exePath = outputPath + ".x";

					std::error_code ec;
					llvm::raw_fd_ostream dest(llPath, ec);
					if (ec) {
						std::cerr << "Could not open output file for LLVM IR: " << ec.message() << "\n";
					} else {
						codegen.getModule()->print(dest, nullptr);
						dest.flush();
						std::cout << "Successfully generated LLVM IR at: " << llPath << "\n";

						std::string command = "clang " + llPath + " -o " + exePath + " -lm";
						int compileResult = std::system(command.c_str());

						if (compileResult == 0) {
							std::cout << "Successfully compiled executable to: " << exePath << "\n";
						} else {
							std::cerr << "Error: Clang failed to compile the generated LLVM module.\n";
						}
					}
				} else {
					std::cerr << "Error: Generated LLVM module failed verification for file: " << path << "\n";
				}
			}
		}
};

#endif // COMPILER_PIPELINE_HPP
