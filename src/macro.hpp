#pragma once

#include <cstdio>
#include <cstdlib>

//@TODO use tinyprintf
#define LUX_LOG(fmt, ...) { \
    std::printf("%s(): " fmt "\n", __func__ __VA_OPT__(,) __VA_ARGS__); }

#define LUX_FATAL(fmt, ...) { \
    std::fprintf(stderr, "FATAL %s(): " fmt "\n", \
            __func__ __VA_OPT__(,) __VA_ARGS__); \
    std::quick_exit(EXIT_FAILURE); }

#define LUX_PANIC(fmt, ...) { \
    std::fprintf(stderr, "PANIC %s(): " fmt "\n", \
            __func__ __VA_OPT__(,) __VA_ARGS__); \
    std::abort(); }

//we don't want our code to call assert from <assert.h>
#undef assert

#ifndef NDEBUG
    #define LUX_ASSERT(expr) { \
        if(!(expr)) LUX_PANIC("assertion `" #expr "` failed"); }
#else
    #define LUX_ASSERT(expr) do {} while(false)
#endif

