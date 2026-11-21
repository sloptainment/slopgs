/* unit_tap.c -- minimal TAP version 14 emitter (see unit_tap.h). Flat output
 * only, no subtests (tap_diag doubles as a group header when one is wanted).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "unit_tap.h"

static int g_n = 0;    /* test points emitted so far -> becomes the plan */
static int g_fail = 0; /* any non-TODO "not ok" seen? -> exit status */

/* Write s to stdout, escaping '\' as '\\' and '#' as '\#' (TAP14 S "Escaping").
 */
static void put_escaped(const char *s)
{
    for (const char *p = s; *p; p++)
    {
        if (*p == '\\' || *p == '#')
            putchar('\\');
        putchar(*p);
    }
}

/* Format desc (fmt/ap) into a buffer, then print "ok N - <desc>" or
 * "not ok N - <desc>", optionally followed by " # <directive>". Directive is
 * printed pre-formatted (already "SKIP reason" / "TODO reason") and escaped
 * like the description. Always consumes one test-point id. */
static void emit_line(int pass, const char *fmt, va_list ap,
                      const char *directive)
{
    /* ponytail: fixed cap, longer descriptions truncate; bump if a real test
     * needs more */
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    printf("%s %d - ", pass ? "ok" : "not ok", ++g_n);
    put_escaped(buf);
    if (directive)
    {
        printf(" # ");
        put_escaped(directive);
    }
    putchar('\n');
}

static void emit_yaml_at(const char *found, const char *wanted,
                         const char *file, int line)
{
    printf("  ---\n  found: %s\n  wanted: %s\n  at:\n    file: %s\n    line: "
           "%d\n  ...\n",
           found, wanted, file, line);
}

void tap_begin(void) { printf("TAP version 14\n"); }

int tap_ok_at(int cond, const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_line(cond, fmt, ap, NULL);
    va_end(ap);
    if (!cond)
    {
        g_fail = 1;
        emit_yaml_at("false", "true", file, line);
    }
    return cond;
}

int tap_is_int_at(long long got, long long want, const char *file, int line,
                  const char *fmt, ...)
{
    int cond = (got == want);
    va_list ap;
    va_start(ap, fmt);
    emit_line(cond, fmt, ap, NULL);
    va_end(ap);
    if (!cond)
    {
        char f[32], w[32];
        snprintf(f, sizeof f, "%lld", got);
        snprintf(w, sizeof w, "%lld", want);
        g_fail = 1;
        emit_yaml_at(f, w, file, line);
    }
    return cond;
}

int tap_is_near_at(double got, double want, double tol, const char *file,
                   int line, const char *fmt, ...)
{
    int cond = fabs(got - want) <= tol;
    va_list ap;
    va_start(ap, fmt);
    emit_line(cond, fmt, ap, NULL);
    va_end(ap);
    if (!cond)
    {
        char f[32], w[32];
        snprintf(f, sizeof f, "%.17g", got);
        snprintf(w, sizeof w, "%.17g", want);
        g_fail = 1;
        emit_yaml_at(f, w, file, line);
    }
    return cond;
}

void tap_skip(const char *reason, const char *fmt, ...)
{
    char dir[300];
    if (reason && *reason)
        snprintf(dir, sizeof dir, "SKIP %s", reason);
    else
        snprintf(dir, sizeof dir, "SKIP");
    va_list ap;
    va_start(ap, fmt);
    emit_line(1, fmt, ap, dir);
    va_end(ap);
}

void tap_todo_at(int cond, const char *reason, const char *file, int line,
                 const char *fmt, ...)
{
    char dir[300];
    if (reason && *reason)
        snprintf(dir, sizeof dir, "TODO %s", reason);
    else
        snprintf(dir, sizeof dir, "TODO");
    va_list ap;
    va_start(ap, fmt);
    emit_line(cond, fmt, ap, dir);
    va_end(ap);
    /* TODO test points never carry a YAML block or affect exit status */
    (void) file;
    (void) line;
}

void tap_diag(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int at_line_start = 1, last_was_nl = 0;
    for (const char *p = buf; *p; p++)
    {
        if (at_line_start)
        {
            printf("# ");
            at_line_start = 0;
        }
        putchar(*p);
        last_was_nl = (*p == '\n');
        if (last_was_nl)
            at_line_start = 1;
    }
    if (!last_was_nl)
        putchar('\n');
}

void tap_bail(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("Bail out! %s\n", buf);
    fflush(stdout);
    exit(1);
}

int tap_done(void)
{
    printf("1..%d\n", g_n);
    fflush(stdout);
    return g_fail ? 1 : 0;
}
