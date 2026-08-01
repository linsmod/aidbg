// test_threads.c - multithreaded target (TestGuid.md 3.4).
//
// Spawns two worker threads that increment a shared counter.  Exercises
// `info threads`, `thread <id>` switching and breakpoints in a multi-threaded
// process.
//
// Build (strategy B, see tests/build.cmd):
//   cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test_threads.exe test_threads.c /link /DEBUG:FULL /DYNAMICBASE:NO

#include <windows.h>
#include <stdio.h>

volatile LONG g_counter = 0;

DWORD WINAPI thread_func(LPVOID param)
{
    (void)param;
    for (int i = 0; i < 5; i++) {
        InterlockedIncrement(&g_counter);
        printf("Thread %lu: %ld\n", GetCurrentThreadId(), g_counter);
        fflush(stdout);
        Sleep(100);
    }
    return 0;
}

int main(void)
{
    HANDLE hThreads[2];
    for (int i = 0; i < 2; i++)
        hThreads[i] = CreateThread(NULL, 0, thread_func, NULL, 0, NULL);
    WaitForMultipleObjects(2, hThreads, TRUE, INFINITE);
    CloseHandle(hThreads[0]);
    CloseHandle(hThreads[1]);
    printf("Final counter: %ld\n", g_counter);
    return 0;
}
