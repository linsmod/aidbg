// test_target.c - minimal debuggee for aidbg smoke tests.
//
// Features exercised:
//   * a long-running loop so we can break, step, inspect
//   * a leaf function we can "finish" out of
//   * a static buffer with a known magic byte sequence (for `search`)
//   * a volatile int3 to verify software breakpoints
//   * a divide-by-zero after many iterations (for AV handling)
//
// Build:
//   cl /nologo /O2 /utf-8 test_target.c /Fe:test_target.exe

#include <windows.h>
#include <stdio.h>
#include <string.h>

static const unsigned char kMagic[] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
};

static unsigned long long g_counter = 0;

__declspec(noinline) static unsigned long long leaf(int a, int b)
{
    return (unsigned long long)(a + b) * (a - b);
}

__declspec(noinline) static void crash(void)
{
    volatile int zero = 0;
    volatile int x = 42;
    printf("crash() x/zero = %d\n", x / zero);
}

int main(void)
{
    printf("test_target starting, pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    for (int i = 0; i < 200; i++) {
        g_counter += leaf(i, 3);
        if (i == 50) {
            /* software breakpoint (int3) in the loop body */
            __debugbreak();
        }
        if ((i & 0x3F) == 0) {
            printf("i=%d counter=%llu\n", i, g_counter);
            fflush(stdout);
        }
        Sleep(50);
    }

    printf("before crash()\n");
    fflush(stdout);
    crash();

    /* verify our magic bytes survived the trip through .rdata */
    if (memcmp(kMagic, "\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE\x11\x22\x33\x44\x55\x66\x77\x88", 16) == 0)
        printf("magic OK\n");
    printf("test_target done, counter=%llu\n", g_counter);
    return 0;
}
