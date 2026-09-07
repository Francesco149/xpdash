#include "log.h"
#include <stdio.h>
#include <stdarg.h>

void agent_log(const char *fmt, ...) {
    FILE *f = fopen("C:\\xpdash\\agent.log", "a");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}
