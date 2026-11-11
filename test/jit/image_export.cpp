// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "image_support.hpp"
#include "jit/llvm/program.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>

namespace {
void export_image(const std::filesystem::path &directory) {
  using namespace cpu12::test::image;
  constexpr std::array<std::uint64_t, 2> ids{7, 9};
  const Source original(0x80);
  const Source changed(0);
  const std::array variants{original.variant(ids[0]), changed.variant(ids[1])};
  const std::array layouts{VariantLayout{.id = ids[0], .memory = layout},
                           VariantLayout{.id = ids[1], .memory = layout}};
  const ImageProgram image({.variants = variants}, layouts);
  std::ofstream header;
  header.exceptions(std::ios::badbit | std::ios::failbit);
  header.open(directory / "entries.hpp");
  header << "// SPDX-License-Identifier: GPL-3.0-only\n#pragma once\n"
            "#include \"cpu12/core.hpp\"\n#include \"jit/block.hpp\"\n#include \"runtime.hpp\"\n"
            "#include <array>\n#include <cstdint>\n"
            "namespace cpu12::test::static_image {\n"
            "using Function = detail::RawNativeResult(Registers *, detail::NativeContext *, "
            "std::uint32_t) noexcept;\nstruct Binding { Address pc; Function *code; };\n";
  for (const auto id : ids) {
    const auto &program = image.program(id);
    const auto object = program.object();
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(directory / ("variant-" + std::to_string(id) + ".o"), std::ios::binary);
    output.write(reinterpret_cast<const char *>(object.data()),
                 static_cast<std::streamsize>(object.size()));
    const auto operations = image.instructions(id);
    for (const auto &operation : operations) {
      header << "extern \"C\" Function " << program.symbol(operation.pc) << ";\n";
    }
    header << "inline const std::array<Binding," << operations.size() << "> variant_" << id
           << "{{\n";
    for (const auto &operation : operations) {
      header << "{" << operation.pc << ",&" << program.symbol(operation.pc) << "},\n";
    }
    header << "}};\n";
  }
  header << "}\n";
}
}

int main(const int argc, char **argv) {
  try {
    cpu12::test::image::require(argc == 2, "Expected the generated fixture directory");
    export_image(argv[1]);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
