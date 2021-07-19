#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/expression.hpp"

#include <stddef.h>
#include <stdint.h>

extern "C" int FUZZ_INIT() {
  return 0; // Non-zero return values are reserved for future use.
}

extern "C" int FUZZ(const uint8_t* Data, size_t Size) {
  std::string_view s(Data, Size);
  auto expr = to<expression>(s);
  if (expr)
    *expr;
  return 0; // Non-zero return values are reserved for future use.
}
