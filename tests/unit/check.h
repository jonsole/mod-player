/* Minimal test helpers: each test is a function; CHECK_EQ records failures. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int check_failures;
static const char *check_current;

#define CHECK_EQ(actual, expected)                                                        \
    do {                                                                                  \
        long long a_ = (long long)(actual), e_ = (long long)(expected);                   \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s (%s:%d): %s = %lld, expected %lld\n", check_current, __FILE__, \
                   __LINE__, #actual, a_, e_);                                            \
            check_failures++;                                                             \
        }                                                                                 \
    } while (0)

#define CHECK(cond) CHECK_EQ(!!(cond), 1)

#define RUN(test)                \
    do {                         \
        check_current = #test;   \
        test();                  \
    } while (0)

#define CHECK_DONE()                                            \
    do {                                                        \
        if (check_failures) {                                   \
            printf("%d check(s) failed\n", check_failures);     \
            return 1;                                           \
        }                                                       \
        printf("all checks passed\n");                          \
        return 0;                                               \
    } while (0)

#endif
