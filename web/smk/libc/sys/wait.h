#pragma once
typedef int pid_t;
pid_t waitpid(pid_t, int *, int); pid_t wait(int *);
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFEXITED(s) 1
