// test_exception.c - exception / signal target (TestGuid.md 3.3).
//
// Run with an argument to select the fault to trigger:
//   test_exception.exe divzero   -> integer divide-by-zero
//   test_exception.exe av        -> access violation (write to NULL)
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_exception.exe test_exception.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>
#include <string.h>

__declspec(noinline) void cause_divide_by_zero(void)
{
    volatile int a = 1, b = 0;
    int c = a / b;   /* divide-by-zero exception */
    (void)c;
}

__declspec(noinline) void cause_access_violation(void)
{
    int* p = NULL;
    *p = 0;          /* access violation */
}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "divzero") == 0) {
        cause_divide_by_zero();
    } else if (argc > 1 && strcmp(argv[1], "av") == 0) {
        cause_access_violation();
    } else {
        printf("Usage: %s [divzero|av]\n", argv[0]);
    }
    return 0;
}
