// exit_test.c - minimal program that exits cleanly after a short delay.
// cl /nologo /O2 /utf-8 exit_test.c /Fe:exit_test.exe
#include <windows.h>
#include <stdio.h>
int main(void)
{
    for (int i = 0; i < 5; i++) { printf("tick %d\n", i); Sleep(100); }
    return 42;
}
