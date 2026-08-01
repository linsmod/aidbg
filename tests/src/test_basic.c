// test_basic.c - basic functionality target (TestGuid.md 3.1).
//
// Exercises: code breakpoints (main/func1/func2), conditional/ignore
// breakpoints, info locals/args, bt backtrace, stepi/nexti, and `search`/
// `strings` (the literal "Hello, aidbg!" lives in .rdata).
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_basic.exe test_basic.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>

int global_var = 42;
volatile int g_loop_index = 0;

__declspec(noinline) int func2(int a, int b)
{
    int local = a + b;
    printf("func2: %d + %d = %d\n", a, b, local);
    return local;
}

__declspec(noinline) int func1(int x)
{
    int y = x * 2;
    return func2(x, y);
}

int main(int argc, char** argv)
{
    printf("Hello, aidbg!\n");
    for (int i = 0; i < 3; i++) {
        g_loop_index = i;
        func1(i);
    }
    global_var = 100;
    printf("global_var = %d\n", global_var);
    return 0;
}
