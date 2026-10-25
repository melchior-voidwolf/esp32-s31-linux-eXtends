/* Dynamic ilp32f runtime smoke for QEMU and the S31 board. */
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;
static volatile sig_atomic_t signal_seen;
static volatile float signal_value;

static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

__attribute__((noinline)) static float fadd3(float a, float b, float c)
{
    return a + b + c;
}

static void on_signal(int unused)
{
    (void)unused;
    signal_value += 1.0f;
    signal_seen = 1;
}

static void test_setjmp(void)
{
#ifdef SOFT_FLOAT_ABI
    jmp_buf environment;
    if (setjmp(environment) == 0)
        longjmp(environment, 1);
    check(1, "setjmp/longjmp control flow (F registers are not ABI-saved)");
#else
    jmp_buf environment;
    const uint32_t one = 0x3f800000u;
    const uint32_t two = 0x40000000u;
    uint32_t restored = 0;
    __asm__ volatile("fmv.w.x fs0, %0" : : "r"(one) : "fs0");
    if (setjmp(environment) == 0) {
        __asm__ volatile("fmv.w.x fs0, %0" : : "r"(two) : "fs0");
        longjmp(environment, 1);
    }
    __asm__ volatile("fmv.x.w %0, fs0" : "=r"(restored));
    check(restored == one, "setjmp/longjmp fs0 restore");
#endif
}

int main(void)
{
    check(fadd3(1.5f, 2.5f, -0.125f) == 3.875f, "float argument/return ABI");
    check(sqrtf(16.0f) == 4.0f, "musl sqrtf");
    volatile double a = 1.5, b = 2.25;
    check(a * b == 3.375, "double without D extension");

    test_setjmp();

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    check(sigaction(SIGUSR1, &action, NULL) == 0, "sigaction setup");
    check(raise(SIGUSR1) == 0 && signal_seen && signal_value == 1.0f,
          "signal and FPU context");

    pid_t child = fork();
    if (child == 0) {
        float value = 1.0f;
        for (int i = 0; i < 10000; i++)
            value += 0.25f;
        _exit(value == 2501.0f ? 0 : 1);
    }
    int status = 0;
    check(child > 0 && waitpid(child, &status, 0) == child
          && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork/wait and FPU context");

    struct timespec now;
    check(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec >= 0,
          "clock_gettime/vDSO");

    if (failures) {
        printf("ABI_RUNTIME_FAIL count=%d\n", failures);
        return 1;
    }
    puts("ABI_RUNTIME_PASS");
    return 0;
}
