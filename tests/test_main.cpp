#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failed = 0;
  for (const auto& c : qtest::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    const int before = qtest::failures();
    c.fn();
    ++run;
    const bool ok = qtest::failures() == before;
    if (!ok) ++failed;
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", c.name);
  }
  std::printf("\n%d tests, %d failed\n", run, failed);
  return failed == 0 && run > 0 ? 0 : 1;
}
