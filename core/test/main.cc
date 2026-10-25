#include <cstdio>
#include <cstring>

#include "test.h"

// Runs every test, or those whose name contains argv[1].
int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& test : lychee::test::Registry()) {
    if (filter && !std::strstr(test.name, filter)) {
      continue;
    }
    const int failures_before = lychee::test::Failures();
    test.body();
    std::printf("%s %s\n",
                lychee::test::Failures() == failures_before ? "PASS" : "FAIL",
                test.name);
    ++run;
  }
  std::printf("%d tests, %d failed expectations\n", run,
              lychee::test::Failures());
  return lychee::test::Failures() ? 1 : 0;
}
