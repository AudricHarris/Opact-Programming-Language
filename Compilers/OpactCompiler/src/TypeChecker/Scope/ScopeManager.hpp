#include "Scope.hpp"
#include <vector>

class ScopeManager
{
	private:
		std::vector<Scope> scopeStacks;
	public:
		void PopStack();
		void AddStack();
};
