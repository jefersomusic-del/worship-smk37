#pragma once
typedef int pid_t;
pid_t fork(void); int unlink(const char *); int usleep(unsigned);
int getpid(void);
int pipe(int *); long write(int, const void *, unsigned long); long read(int, void *, unsigned long); int close(int);
void _exit(int);
