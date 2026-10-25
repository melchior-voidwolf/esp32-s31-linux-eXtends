/* Freestanding S31 ABI/ISA benchmark: no libc, libgcc or variable arguments. */
#include <stdint.h>

#define BENCH_ITERS 1000000u
#define NR_WRITE 64
#define NR_CLOCK_GETTIME64 403
#define CLOCK_MONOTONIC_RAW 4

extern float abi_float_step(float acc, float factor, float increment);
static volatile uint32_t bench_seed = 0x12345678u;
static volatile uint32_t bench_sink;

static inline long syscall3(long number, long arg0, long arg1, long arg2)
{
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a7 __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

struct timespec64 {
    int64_t sec;
    int64_t nsec;
};

static uint32_t monotonic_ns32(void)
{
    struct timespec64 ts;
    if (syscall3(NR_CLOCK_GETTIME64, CLOCK_MONOTONIC_RAW, (long)&ts, 0) != 0)
        return 0;
    /* The caller subtracts immediately; truncation is safe for short runs. */
    return (uint32_t)ts.sec * 1000000000u + (uint32_t)ts.nsec;
}

static uint32_t elapsed_ns(uint32_t start, uint32_t end)
{
    /* Wraparound is harmless because every measured workload is < 4 seconds. */
    return end - start;
}

static uint32_t text_length(const char *text)
{
    uint32_t length = 0;
    while (text[length])
        ++length;
    return length;
}

static void print_text(const char *text)
{
    syscall3(NR_WRITE, 1, (long)text, text_length(text));
}

static void print_u32_width(uint32_t value, uint32_t width)
{
    char buffer[11];
    uint32_t position = sizeof(buffer) - 1;
    uint32_t digits = 0;
    buffer[position] = 0;
    do {
        buffer[--position] = (char)('0' + value % 10u);
        value /= 10u;
        ++digits;
    } while (value);
    while (digits < width) {
        buffer[--position] = '0';
        ++digits;
    }
    print_text(buffer + position);
}

static void print_u32(uint32_t value)
{
    print_u32_width(value, 0);
}

static void print_hex32(uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char output[9];
    for (int i = 0; i < 8; ++i)
        output[i] = digits[(value >> (28 - i * 4)) & 15u];
    output[8] = 0;
    print_text(output);
}

__attribute__((noinline, noipa))
static uint32_t bench_float_local(uint32_t iterations, uint32_t seed)
{
    float acc = 1.0f + (float)(seed & 0xffffu) * 0x1p-20f;
    const float factor = 0.99999994f;
    const float increment = 0x1p-23f;
    union { float f; uint32_t u; } result;
    for (uint32_t i = 0; i < iterations; ++i)
        acc = acc * factor + increment;
    result.f = acc;
    return result.u;
}

__attribute__((noinline, noipa))
static uint32_t bench_float_call(uint32_t iterations, uint32_t seed)
{
    float acc = 1.0f + (float)(seed & 0xffffu) * 0x1p-20f;
    const float factor = 0.99999994f;
    const float increment = 0x1p-23f;
    union { float f; uint32_t u; } result;
    for (uint32_t i = 0; i < iterations; ++i)
        acc = abi_float_step(acc, factor, increment);
    result.f = acc;
    return result.u;
}

static uint32_t portable_clz(uint32_t value)
{
    uint32_t count = 0;
    if (!(value & 0xffff0000u)) { count += 16; value <<= 16; }
    if (!(value & 0xff000000u)) { count += 8; value <<= 8; }
    if (!(value & 0xf0000000u)) { count += 4; value <<= 4; }
    if (!(value & 0xc0000000u)) { count += 2; value <<= 2; }
    if (!(value & 0x80000000u)) { count += 1; }
    return count;
}

static uint32_t portable_ctz(uint32_t value)
{
    uint32_t count = 0;
    if (!(value & 0x0000ffffu)) { count += 16; value >>= 16; }
    if (!(value & 0x000000ffu)) { count += 8; value >>= 8; }
    if (!(value & 0x0000000fu)) { count += 4; value >>= 4; }
    if (!(value & 0x00000003u)) { count += 2; value >>= 2; }
    if (!(value & 0x00000001u)) { count += 1; }
    return count;
}

static uint32_t portable_bswap(uint32_t value)
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
           ((value << 8) & 0x00ff0000u) | (value << 24);
}

__attribute__((noinline, noipa))
static uint32_t bench_bits(uint32_t iterations, uint32_t seed)
{
    uint32_t x = seed ? seed : 1u;
    uint32_t acc = 0x9e3779b9u;
    for (uint32_t i = 0; i < iterations; ++i) {
        uint32_t y;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        y = x | 1u;
#if defined(__riscv_zbb)
        acc += (uint32_t)__builtin_clz(y);
        acc ^= (uint32_t)__builtin_ctz(y);
        acc += __builtin_bswap32(x);
#else
        acc += portable_clz(y);
        acc ^= portable_ctz(y);
        acc += portable_bswap(x);
#endif
        acc = (acc << 7) | (acc >> 25);
        acc &= ~(1u << ((x >> 8) & 31));
        acc |= 1u << (x & 31);
    }
    return acc ^ x;
}

__attribute__((noinline, noipa))
static uint32_t bench_zba(uint32_t iterations, uint32_t seed)
{
    uint32_t x = seed;
    uint32_t y = seed ^ 0xa5a5a5a5u;
    uint32_t z = seed ^ 0x5a5a5a5au;
    for (uint32_t i = 0; i < iterations; ++i) {
        y = x + (y << 1);
        z = y + (z << 2);
        x = z + (x << 3);
    }
    return x ^ y ^ z;
}

__attribute__((noinline, noipa))
static uint32_t bench_int_control(uint32_t iterations, uint32_t seed)
{
    uint32_t x = seed;
    for (uint32_t i = 0; i < iterations; ++i)
        x = x * 1664525u + (i ^ 1013904223u);
    return x;
}

typedef uint32_t (*benchmark_fn)(uint32_t, uint32_t);
struct benchmark {
    const char *name;
    benchmark_fn fn;
};
static const struct benchmark benchmarks[] = {
    { "float", bench_float_local },
    { "abi-call", bench_float_call },
    { "bits", bench_bits },
    { "zba", bench_zba },
    { "int-control", bench_int_control },
};

static void print_result(const char *mode, uint32_t iterations,
                         uint32_t ns, uint32_t checksum)
{
    print_text("RESULT mode=");
    print_text(mode);
    print_text(" iterations=");
    print_u32(iterations);
    print_text(" seconds=");
    print_u32(ns / 1000000000u);
    print_text(".");
    print_u32_width((ns % 1000000000u) / 1000u, 6);
    print_text(" checksum=");
    print_hex32(checksum);
    print_text("\n");
}

int bench_main(void)
{
    uint32_t seed = bench_seed;
    for (uint32_t i = 0; i < sizeof(benchmarks) / sizeof(benchmarks[0]); ++i)
        bench_sink ^= benchmarks[i].fn(10000u, seed);
    for (uint32_t i = 0; i < sizeof(benchmarks) / sizeof(benchmarks[0]); ++i) {
        uint32_t start = monotonic_ns32();
        uint32_t checksum = benchmarks[i].fn(BENCH_ITERS, seed);
        uint32_t ns = elapsed_ns(start, monotonic_ns32());
        if (!ns) {
            print_text("CLOCK_ERROR\n");
            return 2;
        }
        print_result(benchmarks[i].name, BENCH_ITERS, ns, checksum);
    }
    print_text("NOLIB_DONE\n");
    return 0;
}
