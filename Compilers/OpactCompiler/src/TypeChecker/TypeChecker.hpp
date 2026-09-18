#include "Scope/ScopeManager.hpp"
#include <string>
#include <vector>

class Diagnostics
{
	private:
		std::string error;
		int line;
	public:
		void GenerateError();
};

class TypeChecker {
	private:
		ScopeManager scopeManager;
		std::vector<Diagnostics> listErrors;
};	
