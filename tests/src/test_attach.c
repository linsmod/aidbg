// test_attach.c - long-running target for the attach/detach test (TestGuid.md 4.8).
//
// Prints its PID then loops forever (every 250 ms) so an external debugger can
// attach.  The `tick_func` symbol is used as a breakpoint location.
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_attach.exe test_attach.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <windows.h>
#include <stdio.h>

__declspec(noinline) int tick_func(int n)
{
    return n * 2;
}

int main(void)
{
    printf("attach target pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);
    for (int i = 0; i < 240; i++) {
        tick_func(i);
        printf("tick %d\n", i);
        fflush(stdout);
        Sleep(250);
    }
    printf("attach target done\n");
    return 0;
}
