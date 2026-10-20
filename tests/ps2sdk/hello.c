// Minimal ps2sdk program: newlib stdio, kernel threads, timers.
#include <stdio.h>
#include <kernel.h>
#include <time.h>

int main(int argc, char *argv[])
{
    printf("hello: argc=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "(none)");
    printf("hello: thread id %d\n", GetThreadId());
    clock_t c = clock();
    printf("hello: clock %s\n", c >= 0 ? "ok" : "bad");
    printf("hello: done\n");
    return 0;
}
