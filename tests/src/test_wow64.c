// test_wow64.c - 32-bit (x86) target for WoW64 cross-debugging (TestGuid.md 4.16).
//
// aidbg builds as x64 but can debug a 32-bit target. Under WoW64 an int3
// software breakpoint surfaces as STATUS_WX86_BREAKPOINT (0x4000001f) instead
// of STATUS_BREAKPOINT (0x80000003). This target verifies that aidbg consumes
// the breakpoint correctly: it stops once per hit and `continue` moves on
// (it must NOT re-stop on the same address / duplicate the callback).
//
// Build (x86, strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_wow64.exe test_wow64.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <windows.h>
#include <stdio.h>

volatile int g_wow_index = 0;

__declspec(noinline) int wow_target(int x)
{
    int local = x * 3 + 1;
    g_wow_index = x;
    printf("wow_target: %d -> %d\n", x, local);
    return local;
}

int main(int argc, char** argv)
{
    printf("Hello, wow64!\n");
    for (int i = 0; i < 3; i++)
        wow_target(i);
    printf("done, g_wow_index=%d\n", g_wow_index);
    return 0;
}
