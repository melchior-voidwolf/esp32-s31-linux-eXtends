/* Real-system benchmark: syscalls, vDSO, exec and squashfs/MTD reads. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t nanoseconds(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000u + (uint64_t)ts->tv_nsec;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        perror("clock_gettime");
        exit(2);
    }
    return nanoseconds(&ts);
}

static void result(const char *mode, uint64_t start, uint64_t end,
                   uint64_t count, uint64_t sectors)
{
    uint64_t elapsed = end - start;
    printf("RESULT mode=%s count=%" PRIu64 " ns=%" PRIu64
           " ns_per_op=%.1f mtd_read_sectors=%" PRIu64 "\n",
           mode, count, elapsed, count ? (double)elapsed / count : 0.0, sectors);
}

static long mem_available_kb(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char line[256];
    long value = -1;
    if (!file) return -1;
    while (fgets(line, sizeof(line), file))
        if (sscanf(line, "MemAvailable: %ld kB", &value) == 1) break;
    fclose(file);
    return value;
}

static uint64_t mtd_sectors(void)
{
    FILE *file = fopen("/proc/diskstats", "r");
    char line[512], name[64];
    unsigned major, minor;
    uint64_t reads, merged, sectors, total = 0;
    if (!file) {
        if (getenv("BENCH_DEBUG_DISKSTATS")) perror("/proc/diskstats");
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        int rc = sscanf(line, "%u %u %63s %" SCNu64 " %" SCNu64 " %" SCNu64,
                        &major, &minor, name, &reads, &merged, &sectors);
        if (getenv("BENCH_DEBUG_DISKSTATS") && !strncmp(name, "mtdblock", 8))
            fprintf(stderr, "DISKSTATS_DEBUG rc=%d name=%s sectors=%" PRIu64 "\n",
                    rc, name, sectors);
        if (rc == 6 && !strncmp(name, "mtdblock", 8))
            total += sectors;
    }
    fclose(file);
    return total;
}

static void bench_syscalls(uint64_t count)
{
    static volatile pid_t sink;
    uint64_t sectors = mtd_sectors(), start = now_ns();
    for (uint64_t i = 0; i < count; i++) sink = getpid();
    (void)sink;
    uint64_t end = now_ns();
    result("getpid", start, end, count, mtd_sectors() - sectors);
}

static void bench_clock(uint64_t count)
{
    struct timespec ts;
    uint64_t sectors = mtd_sectors(), start = now_ns();
    for (uint64_t i = 0; i < count; i++) (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t end = now_ns();
    result("clock_gettime", start, end, count, mtd_sectors() - sectors);
}

static void bench_exec(uint64_t count)
{
    uint64_t sectors = mtd_sectors(), start = now_ns();
    for (uint64_t i = 0; i < count; i++) {
        pid_t child = fork();
        if (child == 0) {
            execl("/bin/true", "true", (char *)NULL);
            _exit(127);
        }
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child
            || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "fork/exec/wait failed at iteration %" PRIu64 "\n", i);
            exit(1);
        }
    }
    uint64_t end = now_ns();
    result("fork_exec_true", start, end, count, mtd_sectors() - sectors);
}

static void bench_file(const char *path, uint64_t passes)
{
    char buffer[65536];
    uint64_t bytes = 0, checksum = 0;
    uint64_t sectors = mtd_sectors(), start = now_ns();
    for (uint64_t pass = 0; pass < passes; pass++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) { perror(path); exit(1); }
        for (;;) {
            ssize_t got = read(fd, buffer, sizeof(buffer));
            if (got < 0) { perror("read"); exit(1); }
            if (!got) break;
            bytes += (uint64_t)got;
            checksum += (unsigned char)buffer[0] + (unsigned char)buffer[got - 1];
        }
        close(fd);
    }
    uint64_t end = now_ns();
    printf("FILE_CHECKSUM path=%s bytes=%" PRIu64 " checksum=%" PRIu64 "\n",
           path, bytes, checksum);
    result("squashfs_read", start, end, passes, mtd_sectors() - sectors);
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    const char *path = argc > 2 ? argv[2] : "/bin/busybox";
    uint64_t loops = quick ? 10000 : 1000000;
    uint64_t execs = quick ? 2 : 100;
    uint64_t passes = quick ? 1 : 30;
    printf("SYSTEM_BENCH_START mem_available_kb=%ld quick=%d\n",
           mem_available_kb(), quick);
    bench_syscalls(loops);
    bench_clock(loops);
    bench_exec(execs);
    bench_file(path, passes);
    printf("SYSTEM_BENCH_DONE mem_available_kb=%ld\n", mem_available_kb());
    return 0;
}
