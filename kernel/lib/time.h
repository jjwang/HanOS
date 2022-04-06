/**-----------------------------------------------------------------------------

 @file    time.h
 @brief   Definition of time related data structures and functions
 @details
 @verbatim

  e.g., time value, time zone and time information.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <core/hpet.h>

#define sleep(x)            hpet_nanosleep(MILLIS_TO_NANOS(x))

#define SECONDS_TO_NANOS(x) ((x)*1000000000ULL)
#define MILLIS_TO_NANOS(x)  ((x)*1000000ULL)
#define MICROS_TO_NANOS(x)  ((x)*1000ULL)
#define NANOS_TO_SECONDS(x) ((x) / 1000000000ULL)
#define NANOS_TO_MILLIS(x)  ((x) / 1000000ULL)
#define NANOS_TO_MICROS(x)  ((x) / 1000ULL)

typedef uint64_t time_t;

typedef struct {
    int minuteswest;     /* minutes west of Greenwich */
    int dsttime;         /* type of DST correction */
} timezone_t;

typedef struct {
    int sec;    /* Seconds (0-60) */
    int min;    /* Minutes (0-59) */
    int hour;   /* Hours (0-23) */
    int mday;   /* Day of the month (1-31) */
    int mon;    /* Month (0-11) */
    int year;   /* Year - 1900 */
    int wday;   /* Day of the week (0-6, Sunday = 0) */
    int yday;   /* Day in the year (0-365, 1 Jan = 0) */
    int isdst;  /* Daylight saving time */
} tm_t;

void localtime(const time_t* timep, tm_t* tm);

