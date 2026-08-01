#include <windows.h>
#include <stdio.h>

int main(void)
{
    FILE* f = fopen("dbgbrk_before.flag", "w");
    if (f) { fputs("before\n", f); fclose(f); }
    DebugBreak();
    f = fopen("dbgbrk_after.flag", "w");
    if (f) { fputs("after\n", f); fclose(f); }
    return 0;
}
