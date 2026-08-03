// test_string.c - examine string formats (x/s, x/hs, x/ws) and local vars.
//
// Exercises (handover8):
//   - `x/s <ascii>` reads a full NUL-terminated 8-bit string (not just the
//     first examine unit)
//   - `x/hs <wide>` decodes a UTF-16 string (GDB: h = 16-bit char strings)
//   - `x/ws` decodes a UTF-32 string (GDB: w = 32-bit char strings)
//   - `x/hs <local>` / `dump <local> N` resolve a local to its VALUE (GDB
//     treats the local's value as the address, e.g. a pointer param)
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_string.exe test_string.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <stdio.h>

const char g_ascii[] = "Hello, aidbg!";
const wchar_t g_wide[] = L"Hello, wide aidbg!";

__declspec(noinline) int show(const wchar_t* pw, const char* pa)
{
    printf("%ls %s\n", pw, pa);
    return (int)(*pw == L'H');
}

int main(void)
{
    const wchar_t* pwide = g_wide;
    const char* pascii = g_ascii;
    return show(pwide, pascii);
}
