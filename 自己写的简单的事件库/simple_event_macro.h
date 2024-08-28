#include <stdio.h>

#define LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define TRACE LOG("%s", __func__);

#define INTERVAL_LOG(interval, fmt, ...) {\
    static int count = 0;\
    if (count>=interval) {LOG(fmt, __VA_ARGS__); count = 0;}\
    count++;\
}

// #define INTERVAL_LOG(interval, fmt, ...) 

