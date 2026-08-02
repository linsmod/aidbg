// test_source_step.c - source-level step / next target.
//
// Exercises GDB `step` (source step into) and `next` (source step over):
//   - `step` enters callee() at its first source line
//   - `next` skips the callee() call in one step
//   - `next`/`step` across a for-loop and its body
//   - `step`/`next` with a repeat count N
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_source_step.exe test_source_step.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>

__declspec(noinline) int callee(int a, int b)
{
    int sum = a + b;
    return sum;
}

__declspec(noinline) int caller(void)
{
    int x = 10;
    return callee(x, 20);
}

int main(void)
{
    int v = 0;
    v = callee(1, 2);
    for (int i = 0; i < 3; i++) {
        v += i;
    }
    printf("v = %d\n", v);
    return v;
}
