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
#include <functional>
#include <cstdio>
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

		// Resolves an import path relative to the file that imports it.
		static std::string resolveImportPath(const std::string& path, const std::string& parentPath = "") {
			fs::path targetPath(path);
			if (!parentPath.empty() && targetPath.is_relative()) {
				fs::path parentDir = fs::path(parentPath).parent_path();
				return (parentDir / targetPath).lexically_normal().string();
			}
			return targetPath.lexically_normal().string();
		}

		// Accepts imported path and optional parent file path to resolve relative imports
		void enqueueFile(const std::string& path, const std::string& parentPath = "") {
			std::string canonicalString = resolveImportPath(path, parentPath);

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
			struct Built { std::string llPath; bool hasMain = false; };
			std::unordered_map<std::string, Built> built;

			// 1) Type check + generate one .ll file per module.
			for (auto& [path, module] : parsedModules) {
				if (!module.astRoot) continue;

				TypeChecker t(this, path);
				t.initialize(module.astRoot);

				llvm::LLVMContext llvmContext;
				CodeGenerator codegen(path, llvmContext);

				// Make the public functions of imported modules callable.
				for (const auto& imp : module.modulePath) {
					const Module* dep = getModuleByPath(resolveImportPath(imp, path));
					if (dep && dep->astRoot) codegen.declareImportedFunctions(dep->astRoot.get());
				}

				module.astRoot->accept(codegen);

				if (codegen.hasErrors()) {
					std::cerr << "Error: code generation failed for file: " << path << "\n";
					continue;
				}
				if (llvm::verifyModule(*codegen.getModule(), &llvm::errs())) {
					std::cerr << "Error: Generated LLVM module failed verification for file: " << path << "\n";
					continue;
				}

				std::string base = path;
				size_t dotPos = base.find_last_of('.');
				if (dotPos != std::string::npos) base = base.substr(0, dotPos);
				std::string llPath = base + ".ll";

				std::error_code ec;
				llvm::raw_fd_ostream dest(llPath, ec);
				if (ec) {
					std::cerr << "Could not open output file for LLVM IR: " << ec.message() << "\n";
					continue;
				}
				codegen.getModule()->print(dest, nullptr);
				dest.flush();

				llvm::Function* mainFn = codegen.getModule()->getFunction("main");
				built[path] = { llPath, mainFn && !mainFn->isDeclaration() };
			}

			// 2) Link: every module that defines Main becomes an executable,
			//    together with all the modules it (transitively) imports.
			for (auto& [path, b] : built) {
				if (!b.hasMain) continue;

				std::vector<std::string> files;
				std::unordered_set<std::string> seen;
				bool complete = true;

				std::function<void(const std::string&)> collect = [&](const std::string& p) {
					if (!seen.insert(p).second) return;
					auto it = built.find(p);
					if (it == built.end()) { complete = false; return; }
					files.push_back(it->second.llPath);
					for (const auto& imp : parsedModules.at(p).modulePath)
						collect(resolveImportPath(imp, p));
				};
				collect(path);

				if (!complete) {
					std::cerr << "Error: cannot link '" << path << "': an imported module failed to compile.\n";
					continue;
				}

				std::string exePath = path.substr(0, path.find_last_of('.')) + ".x";
				std::string command = "clang -Wno-override-module";
				for (const auto& f : files) command += " \"" + f + "\"";
				command += " -o \"" + exePath + "\" -lm";

				if (std::system(command.c_str()) == 0)
					std::cout << "Successfully compiled executable to: " << exePath << "\n";
				else
					std::cerr << "Error: Clang failed to compile the generated LLVM modules.\n";
			}

			// 3) Clean up the intermediate .ll files.
			for (auto& [path, b] : built) std::remove(b.llPath.c_str());
		}
};

#endif // COMPILER_PIPELINE_HPP
