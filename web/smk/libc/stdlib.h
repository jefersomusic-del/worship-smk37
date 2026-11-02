#pragma once
#include <stddef.h>
void *malloc(size_t); void *calloc(size_t, size_t); void *realloc(void *, size_t); void free(void *);
int atoi(const char *); long strtol(const char *, char **, int); double atof(const char *); double strtod(const char *, char **);
char *getenv(const char *); void exit(int); void abort(void); int abs(int);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
