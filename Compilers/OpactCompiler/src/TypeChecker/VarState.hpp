#include <string>
#include <system_error>
#include <vector>

enum class StatusOwnership
{
	OWNED,
	MOVED,
	BORROWED_SHARED,
	BORROWED_MUT
};

enum class TypeVar {
	PRIMITIVE,
	REFERENCE,
	CUSTOM
};

struct TypeInfo {
	TypeVar type;
	std::string typeName;
};

class VarState
{
	private:
		std::string name;
		int declaredLoc;
		TypeInfo type;
		StatusOwnership varStatus;

	public:
		bool isVariable(std::string name);
		void changeStatus(StatusOwnership newStatus);
		TypeInfo getType();
		StatusOwnership getStatus();
};

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

class ScopeManager
{
	private:
		std::vector<VarState> scopeStacks;
	public:
		void PopStack();
		void AddStack();
};

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
