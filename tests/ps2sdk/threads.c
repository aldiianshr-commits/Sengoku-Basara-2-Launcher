// EE kernel coverage: threads, priorities, semaphores, sleep/wakeup,
// alarms, interrupt handlers and the vsync interrupt.
#include <stdio.h>
#include <kernel.h>

static int sema, order[8], norder;
static volatile int vsyncs, alarmHits;
static char stack[2][0x1000] __attribute__((aligned(16)));

static void worker(void *arg)
{
    int id = (int)arg;
    WaitSema(sema);
    order[norder++] = id;
    SignalSema(sema);
    ExitThread();
}

static int vsyncHandler(int cause)
{
    vsyncs++;
    ExitHandler();
    return 0;
}

static void alarmCb(s32 id, u16 time, void *common)
{
    alarmHits++;
    iWakeupThread((int)common);
}

int main(void)
{
    ee_sema_t s = {0};
    ee_thread_t t = {0};
    int tid[2], i, h;
    s.init_count = 0;
    s.max_count = 1;
    sema = CreateSema(&s);
    printf("threads: sema %d\n", sema > 0);
    for (i = 0; i < 2; i++) {
        t.func = worker;
        t.stack = stack[i];
        t.stack_size = sizeof(stack[i]);
        t.gp_reg = &_gp;
        t.initial_priority = 10 + i;
        tid[i] = CreateThread(&t);
        StartThread(tid[i], (void *)(i + 1));
    }
    printf("threads: created %d %d\n", tid[0] > 0, tid[1] > 0);
    SignalSema(sema);
    // Lower our priority so the workers run.
    ChangeThreadPriority(GetThreadId(), 20);
    printf("threads: order %d %d count %d\n", order[0], order[1], norder);
    h = AddIntcHandler(INTC_VBLANK_S, vsyncHandler, 0);
    EnableIntc(INTC_VBLANK_S);
    SetAlarm(200, alarmCb, (void *)GetThreadId());
    SleepThread();
    printf("threads: alarm woke us %d\n", alarmHits);
    while (vsyncs < 10) ;
    printf("threads: vsync handler ran %d\n", vsyncs >= 10);
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, h);
    printf("threads: done\n");
    return 0;
}
