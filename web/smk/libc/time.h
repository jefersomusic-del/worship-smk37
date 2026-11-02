#pragma once
typedef long time_t; typedef long clock_t;
struct timespec { time_t tv_sec; long tv_nsec; };
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC 1000000
int clock_gettime(int, struct timespec *); clock_t clock(void); time_t time(time_t *);
