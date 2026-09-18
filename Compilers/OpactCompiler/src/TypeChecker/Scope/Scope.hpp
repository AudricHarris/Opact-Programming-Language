#include "../VarState/VarState.hpp"
#include <vector>

class Scope
{
	private:
		std::vector<VarState> variables;
	public:
		bool addVariable(VarState v);
		bool hasVariable(std::string varName);
		VarState getVariable(std::string varName);
		bool removeVariabe(VarState v);
		bool moveVariable(VarState v);
};
