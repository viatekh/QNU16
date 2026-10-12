#pragma once

// Minimal self-contained test harness (no external dependencies).

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace qtest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int n = 0;
  return n;
}

struct Registrar {
  Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace qtest

#define QT_CAT2(a, b) a##b
#define QT_CAT(a, b) QT_CAT2(a, b)
#define TEST(name)                                                        \
  static void name();                                                     \
  static qtest::Registrar QT_CAT(reg_, name)(#name, name);                \
  static void name()

#define CHECK(cond)                                                                   \
  do {                                                                                \
    if (!(cond)) {                                                                    \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
      ++qtest::failures();                                                            \
    }                                                                                 \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                    \
  do {                                                                                           \
    const double qa = (a), qb = (b);                                                             \
    if (!(std::fabs(qa - qb) <= (tol))) {                                                        \
      std::printf("    FAIL %s:%d: %s = %g, expected %g +/- %g\n", __FILE__, __LINE__, #a, qa, qb, \
                  double(tol));                                                                  \
      ++qtest::failures();                                                                       \
    }                                                                                            \
  } while (0)
