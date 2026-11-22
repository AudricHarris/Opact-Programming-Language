# Adding features to the code generator

Hi, if you want to modify the work of codegen here is a small doc.
This is probably the hardest part of the compiler by a mile. I have given a small guide on
how you can expand the codegen to implement your own expressions or built in
You will have to bear with me on this one.

## Where things live

|----------------------------------------------|---------------------------------------|
| I want to change...                          | File                                  |
|----------------------------------------------|---------------------------------------|
| a builtin (`Printf`, `Range`, `Sqrt`...)     | `Builtins/*.cpp`                      |
| how a language type maps to LLVM             | `Core/Types.cpp` (`mapType`)          |
| implicit conversions (int -> float, ...)     | `Core/Types.cpp` (`coerce`, `unify`)  |
| `+ - * / == <` etc.                          | `Visitors/Operators.cpp`              |
| `if / while / loop / return`                 | `Visitors/ControlFlow.cpp`            |
| `for x in ...`                               | `Visitors/ForLoop.cpp`                |
| variables / assignment                       | `Visitors/Variables.cpp`              |
| arrays (`a[i]`, literals, `?` dimensions)    | `Visitors/Arrays.cpp`, `DynArray.*`   |
| structs (`Foo { .. }`, `a.b`)                | `Visitors/Structs.cpp`                |
| function calls                               | `Visitors/Calls.cpp`                  |
| heap ownership / auto-free                   | `Core/Scopes.cpp`                     |
| unsigned checks, constant folding            | `Core/Unsigned.cpp`                   |
| `\n`, `\e`, `\x1b` handling in strings       | `StringUtil.*`                        |
|--------------------------------------------------------------------------------------|

Every `.cpp` includes `../CodeGenerator.hpp`, which declares the whole class,
so if you believe a function is at it's wrong place you can move it. and you can even create
idk `CustomBehaviour/*.cpp` and implement all your stuff in there

---

## Recipe 1 - add a builtin that wraps a C function (1 line)

Open the module it belongs to (or `Builtins/Libc.cpp`) and add:

```cpp
registerExternal("Sleep", "sleep", i32, { i32 });   // Sleep(2) -> sleep(2)
registerExternal("Sin",   "sin",   f64, { f64 });
```

Arguments are coerced to the declared types, the C function is declared in
the module only if the program uses it. Pass `true` as a last argument for a
variadic C function.

## Recipe 2 - add a builtin with custom IR (inside an existing module)

Add a `registerBuiltin(name, lambda)` call to the module's `register...()`
function. The lambda has access to everything private (`emit`, `error`,
`coerce`, `unify`, `builder`, `module`, `createEntryAlloca`, ...):

```cpp
registerBuiltin("Square", [this](CallExpr& call) -> llvm::Value* {
	if (call.arguments.size() != 1) { error("Square: expected 1 argument"); return nullptr; }
	llvm::Value* v = emit(call.arguments[0].get());
	if (!v) return nullptr;
	return v->getType()->isFloatingPointTy() ? builder.CreateFMul(v, v, "sq")
	                                         : builder.CreateMul(v, v, "sq");
});
```

Rules of a handler: return the resulting `llvm::Value*`, or `nullptr` for
"void" or after reporting with `error(...)`. No header change is needed.

## Recipe 3 - add a new builtin MODULE (a new file)

Example: `Builtins/Math.cpp` is already wired up; copy it.

1. Create `Builtins/Time.cpp`:
   ```cpp
   #include "../CodeGenerator.hpp"

   void CodeGenerator::registerTimeBuiltins() {
       registerExternal("Time", "time", builder.getInt64Ty(), { builder.getPtrTy() });
   }
   ```
2. Declare it in `CodeGenerator.hpp` (private section, "Builtins internals"):
   ```cpp
   void registerTimeBuiltins();
   ```
3. Call it in `Builtins/Builtins.cpp` -> `registerBuiltins()`:
   ```cpp
   registerTimeBuiltins();
   ```
4. Re-run CMake (the `GLOB_RECURSE` picks the new file up), build.

User functions with the same name win over builtins (see `Visitors/Calls.cpp`).

## Recipe 4 - add a new primitive type

1. Add the enumerator to `DataType` (Parser/Ast.hpp) and teach the parser.
2. `Core/Types.cpp` -> `mapType`: add a `case DataType::YourType:`.
3. If it needs conversions: `coerce` / `unify` (same file).
4. If it should print: `Builtins/Format.cpp` -> `appendFormatArg`.

## Recipe 5 - add a new AST node (new syntax)

1. Define the node in `Parser/Ast.hpp` and add `virtual void visit(YourExpr&) = 0;`
   to `ASTVisitor` (and to every other visitor you have).
2. Declare the override in `CodeGenerator.hpp` under "Visitors":
   ```cpp
   void visit(YourExpr& node) override;
   ```
3. Implement it in the matching `Visitors/*.cpp` (or a new one):
   ```cpp
   void CodeGenerator::visit(YourExpr& node) {
       llvm::Value* v = emit(node.child.get());   // evaluate a child
       if (!v) return;
       lastValue = builder.CreateNeg(v, "neg");   // the node's result
   }
   ```
   Convention: the result of a node goes in `lastValue` (`nullptr` if it has none).
4. The placeholder `visit(GenericCallExpr&) {}` etc. in the header are the
   "not implemented yet" nodes; replace the `{}` with a declaration and implement them
   the same way.

## Adding a new source file

`CMakeLists.txt` uses `file(GLOB_RECURSE ...)`, so just re-run CMake.
Without CMake, add the new `.cpp` to your build list.
