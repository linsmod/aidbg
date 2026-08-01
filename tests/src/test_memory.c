// test_memory.c - memory / watchpoint target (TestGuid.md 3.2).
//
// A global `int data[100]` written and read through helper functions so that
// memory breakpoints (`watch` / `mbreak`) and `dump`/`x` can be exercised.
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_memory.exe test_memory.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>
#include <string.h>

int data[100];

__declspec(noinline) void write_data(int idx, int val) { data[idx] = val; }
__declspec(noinline) int read_data(int idx)             { return data[idx]; }

int main(void)
{
    memset(data, 0, sizeof(data));
    write_data(10, 0x12345678);
    int v = read_data(10);
    printf("v = 0x%x\n", v);
    return 0;
}
