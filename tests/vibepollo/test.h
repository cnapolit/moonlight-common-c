#pragma once

#include <stdio.h>
#include <stdlib.h>

static int testFailures;

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            testFailures++; \
        } \
    } while (0)

#define TEST_RESULT() (testFailures == 0 ? (printf("OK\n"), 0) : (fprintf(stderr, "%d failures\n", testFailures), 1))
