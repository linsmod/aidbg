// test_vars.c - local variables / expressions / frame navigation target.
//
// Exercises (handover7):
//   - `print <local>` reads a local variable from the current frame
//   - `print <expr>` evaluates arithmetic / dereference expressions
//   - `condition <id> <local> == N` stops only when the local matches
//   - `set <local> = <val>` writes a local variable
//   - `frame N` / `up` / `down` switch the frame for info locals / print
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_vars.exe test_vars.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>

volatile int g_vtick = 0;

__declspec(noinline) int level2(int a, int b)
{
    int local_sum = a + b;
    int local_prod = a * b;
    int local_total = local_sum + local_prod;
    g_vtick = local_total;
    return local_total;
}

__declspec(noinline) int level1(int x)
{
    int local_x2 = x * 2;
    int local_arr[4];
    local_arr[0] = 10;
    local_arr[1] = 20;
    local_arr[2] = 30;
    local_arr[3] = 40;
    int local_ret = level2(local_x2, local_arr[2]);
    g_vtick += local_ret;
    return local_ret;
}

int main(void)
{
    int v = 0;
    v = level1(3);
    g_vtick += v;
    printf("v = %d, tick = %d\n", v, g_vtick);
    return v;
}
