#include "vast/system/application.hpp"
#include "vast/system/default_configuration.hpp"

#include <stddef.h>
#include <stdint.h>

extern "C" int FUZZ_INIT() {
  return 0; // Non-zero return values are reserved for future use.
}

extern "C" int FUZZ(const char* Data, size_t Size) {
  vast::system::default_configuration cfg;
  auto [root, root_factory] = vast::system::make_application("vast");
  if (!root)
    return 0;
  std::string s(Data, Size);
  std::vector<std::string> command_line{"--node", "export", "json", s.c_str()};
  auto invocation = parse(*root, command_line.begin(), command_line.end());
  return 0; // Non-zero return values are reserved for future use.
}
