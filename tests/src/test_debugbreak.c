#include <windows.h>
#include <stdio.h>
#include <string.h>

static void write_flag(const char* path, const char* text)
{
    FILE* f = fopen(path, "w");
    if(f)
    {
        fputs(text, f);
        fclose(f);
    }
}

int main(int argc, char** argv)
{
    if(argc > 1 && strcmp(argv[1], "raise") == 0)
    {
        write_flag("raise_before.flag", "before\n");
        __try
        {
            RaiseException(STATUS_BREAKPOINT, 0, 0, NULL);
        }
        __except(GetExceptionCode() == STATUS_BREAKPOINT
                     ? EXCEPTION_EXECUTE_HANDLER
                     : EXCEPTION_CONTINUE_SEARCH)
        {
            write_flag("raise_handler.flag", "handled\n");
        }
        write_flag("raise_after.flag", "after\n");
        return 0;
    }

    write_flag("dbgbrk_before.flag", "before\n");
    DebugBreak();
    write_flag("dbgbrk_after.flag", "after\n");
    return 0;
}
