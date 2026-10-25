// A minimal test harness, so the core needs no test framework dependency.
//
//   TEST(MediaClock, Pauses) { EXPECT_EQ(clock.Now(), 0); }

#ifndef LYCHEE_TEST_TEST_H_
#define LYCHEE_TEST_TEST_H_

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace lychee::test {

struct TestCase {
  const char* name;
  std::function<void()> body;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    Registry().push_back({name, std::move(body)});
  }
};

template <typename A, typename B>
void ExpectEq(const A& a, const B& b, const char* expr, const char* file,
              int line) {
  if (a == b) {
    return;
  }
  std::ostringstream message;
  message << a << " != " << b;
  std::fprintf(stderr, "%s:%d: EXPECT_EQ(%s): %s\n", file, line, expr,
               message.str().c_str());
  ++Failures();
}

}  // namespace lychee::test

#define TEST(suite, name)                                          \
  static void suite##_##name();                                    \
  static ::lychee::test::Registrar suite##_##name##_registrar(     \
      #suite "." #name, suite##_##name);                           \
  static void suite##_##name()

#define EXPECT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "%s:%d: EXPECT_TRUE(%s)\n", __FILE__,         \
                   __LINE__, #cond);                                     \
      ++::lychee::test::Failures();                                      \
    }                                                                    \
  } while (0)

#define EXPECT_EQ(a, b) \
  ::lychee::test::ExpectEq((a), (b), #a ", " #b, __FILE__, __LINE__)

// |a| within |tolerance| of |b|.
#define EXPECT_NEAR(a, b, tolerance) EXPECT_TRUE(std::llabs((a) - (b)) <= (tolerance))

#endif  // LYCHEE_TEST_TEST_H_
