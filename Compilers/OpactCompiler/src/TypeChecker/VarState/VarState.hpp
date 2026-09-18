#include <string>

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
