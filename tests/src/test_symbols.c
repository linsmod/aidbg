// test_symbols.c - symbol / source-location target (TestGuid.md 3.5).
//
// Exercises `break <symbol>` (break add), `list` (source listing) and
// `bt` (backtrace with file:line).
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_symbols.exe test_symbols.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>

__declspec(noinline) int add(int a, int b)
{
    int result = a + b;
    return result;
}

int main(void)
{
    int x = 3, y = 4;
    int z = add(x, y);
    printf("z = %d\n", z);
    return 0;
}
