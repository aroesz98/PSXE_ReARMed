/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "log.h"

/* Include for MCU debug console */
#include "fsl_debug_console.h"
#include <stdio.h>
#include <string.h>

#define MAX_CALLBACKS 32

/* Rate limiting for repetitive messages */
#define MAX_RATE_LIMIT_ENTRIES 10
typedef struct
{
    char message[64];
    uint32_t count;
    uint32_t last_printed_count;
} rate_limit_entry_t;

static rate_limit_entry_t rate_limit_table[MAX_RATE_LIMIT_ENTRIES];
static int32_t rate_limit_index = 0;

typedef struct
{
    log_LogFn fn;
    void *udata;
    int32_t level;
} Callback;

static struct
{
    void *udata;
    log_LockFn lock;
    int32_t level;
    bool quiet;
    Callback callbacks[MAX_CALLBACKS];
} L;

static const char *level_strings[] = {
    "trace", "debug", "info", "warn", "error", "fatal"};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
    "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"};
#endif

/* Rate limiting function for repetitive messages */
static int32_t should_rate_limit(const char *message)
{
    /* Only rate limit debug messages */
    if (!message)
        return 0;

    /* Check if this message should be rate limited */
    for (int32_t i = 0; i < MAX_RATE_LIMIT_ENTRIES; i++)
    {
        if (strncmp(rate_limit_table[i].message, message, sizeof(rate_limit_table[i].message) - 1) == 0)
        {
            rate_limit_table[i].count++;
            /* Print every 100th occurrence */
            if (rate_limit_table[i].count % 100 == 1)
            {
                rate_limit_table[i].last_printed_count = rate_limit_table[i].count;
                return 0; /* Don't rate limit */
            }
            return 1; /* Rate limit this message */
        }
    }

    /* New message, add to table */
    int32_t idx = rate_limit_index % MAX_RATE_LIMIT_ENTRIES;
    strncpy(rate_limit_table[idx].message, message, sizeof(rate_limit_table[idx].message) - 1);
    rate_limit_table[idx].message[sizeof(rate_limit_table[idx].message) - 1] = '\0';
    rate_limit_table[idx].count = 1;
    rate_limit_table[idx].last_printed_count = 1;
    rate_limit_index++;

    return 0; /* Don't rate limit first occurrence */
}

static void stdout_callback(log_Event *ev)
{
    char buffer[256]; /* Buffer for formatted message */

    /* Format the variable arguments into a buffer first */
    vsnprintf(buffer, sizeof(buffer), ev->fmt, ev->ap);

    /* Apply rate limiting for debug and error messages */
    if ((ev->level == LOG_DEBUG || ev->level == LOG_ERROR) && should_rate_limit(buffer))
    {
        return; /* Skip this message due to rate limiting */
    }

#ifdef LOG_USE_COLOR
    PRINTF("psx: %s%s\x1b[0m \x1b[90m%s:\x1b[0m ",
           level_colors[ev->level], level_strings[ev->level],
           ev->file);
#else
    PRINTF("psx: %s %s: ",
           level_strings[ev->level], ev->file);
#endif

    PRINTF("%s\r\n", buffer);
}

static void file_callback(log_Event *ev)
{
    char buffer[256]; /* Buffer for formatted message */

    /* Format the variable arguments into a buffer */
    vsnprintf(buffer, sizeof(buffer), ev->fmt, ev->ap);

    /* Note: strftime might not be available on MCU, using simplified format */
    PRINTF("psx: %s %s:%d: %s\r\n",
           level_strings[ev->level], ev->file, ev->line, buffer);
}

static void lock(void)
{
    if (L.lock)
    {
        L.lock(true, L.udata);
    }
}

static void unlock(void)
{
    if (L.lock)
    {
        L.lock(false, L.udata);
    }
}

const char *log_level_string(int32_t level)
{
    return level_strings[level];
}

void log_set_lock(log_LockFn fn, void *udata)
{
    L.lock = fn;
    L.udata = udata;
}

void log_set_level(int32_t level)
{
    L.level = level;
}

void log_set_quiet(bool enable)
{
    L.quiet = enable;
}

int32_t log_add_callback(log_LogFn fn, void *udata, int32_t level)
{
    for (int32_t i = 0; i < MAX_CALLBACKS; i++)
    {
        if (!L.callbacks[i].fn)
        {
            L.callbacks[i] = (Callback){fn, udata, level};
            return 0;
        }
    }
    return -1;
}

int32_t log_add_fp(FILE *fp, int32_t level)
{
    return log_add_callback(file_callback, fp, level);
}

static void init_event(log_Event *ev, void *udata)
{
    /* Skip time initialization on MCU to avoid issues with time.h */
    ev->time = NULL;
    ev->udata = udata;
}

void log_log(int32_t level, const char *file, int32_t line, const char *fmt, ...)
{
    log_Event ev = {
        .fmt = fmt,
        .file = file,
        .line = line,
        .level = level,
    };

    lock();

    if (!L.quiet && level >= L.level)
    {
        init_event(&ev, stderr);
        va_start(ev.ap, fmt);
        stdout_callback(&ev);
        va_end(ev.ap);
    }

    for (int32_t i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++)
    {
        Callback *cb = &L.callbacks[i];
        if (level >= cb->level)
        {
            init_event(&ev, cb->udata);
            va_start(ev.ap, fmt);
            cb->fn(&ev);
            va_end(ev.ap);
        }
    }

    unlock();
}
