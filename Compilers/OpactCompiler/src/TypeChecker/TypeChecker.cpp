#include "TypeChecker.hpp"
#include "Parser/Ast.hpp"
#include <iostream>

void TypeChecker::initialize(const ExprPtr& m)
{
	if (m) {
		std::cout << "Initializing Module at address: " << m.get() << "\n";
	}
}
