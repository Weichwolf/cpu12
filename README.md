# cpu12

Independent CPU12/HC12 emulator with an x86-64 JIT.

C++23 · Linux x86-64 · GPL-3.0-only

## Build

Requires LLVM 19, CMake, Ninja, clang-format-19 and clang-tidy-19.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The `strict` preset enables ASan/UBSan.

## Use

```cmake
add_subdirectory(path/to/cpu12)
target_link_libraries(your_app PRIVATE cpu12::core)
```

```cpp
#include <cpu12/core.hpp>
#include <cpu12/isa.hpp>
#include <cpu12/memory.hpp>

#include <algorithm>
#include <array>

int main() {
  std::array<cpu12::Byte, cpu12::address_bytes> ram{};
  constexpr auto program = cpu12::isa::program(cpu12::isa::nop, cpu12::isa::stop);
  std::ranges::copy(program, ram.begin());
  cpu12::Core core(cpu12::Memory::unified(ram));
  const auto result = core.run();
  return result.fault == cpu12::Fault::none ? 0 : 1;
}
```

Memory is borrowed and must outlive the core. One execution thread owns each
core. The example executes NOP and STOP; a consumer supplies its own program and
optional I/O. Unsupported instructions or modes report explicit faults.

[License](LICENSE)
