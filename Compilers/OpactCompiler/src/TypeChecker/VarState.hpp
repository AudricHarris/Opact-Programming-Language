#include <string>
#include <vector>
#include "Scope/ScopeManager.hpp"


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
