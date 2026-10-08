#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "xdec/binary/image.h"
#include "xdec/binary/target_profile.h"

using namespace xdec;
using namespace xdec::binary;

namespace {

[[nodiscard]] BinaryImage machoWithImports(std::initializer_list<const char*> names) {
  ImageContents contents;
  contents.format = BinaryFormat::MachO;
  contents.arch = Arch::AArch64;
  contents.pointerBits = 64;
  for (const char* name : names) {
    Symbol symbol;
    symbol.name = name;
    symbol.defined = false;
    symbol.fromDynamic = true;
    contents.symbols.push_back(std::move(symbol));
  }
  return BinaryImage{std::move(contents)};
}

[[nodiscard]] bool has(const TargetProfile& profile, const char* preset) {
  return std::find(profile.typePresets.begin(), profile.typePresets.end(), preset) !=
         profile.typePresets.end();
}

}  // namespace

TEST_CASE("Mach-O imports select optional iOS declaration layers",
          "[binary][target-profile]") {
  BinaryImage image = machoWithImports(
      {"_SecTaskCreateWithAuditToken", "__os_log_impl", "_MGCopyAnswer"});
  const TargetProfile profile = inferTargetProfile(image);

  REQUIRE(profile.typePresets.front() == "ios-sdk");
  REQUIRE(has(profile, "ios-sdk-security"));
  REQUIRE(has(profile, "ios-sdk-oslog"));
  REQUIRE(has(profile, "ios-sdk-spi"));
}

TEST_CASE("an iOS image without framework imports keeps only the public bundle",
          "[binary][target-profile]") {
  BinaryImage image = machoWithImports({"_malloc", "_mach_msg"});
  const TargetProfile profile = inferTargetProfile(image);
  REQUIRE(profile.typePresets == std::vector<std::string>{"ios-sdk"});
}
