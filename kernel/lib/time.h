/**-----------------------------------------------------------------------------

 @file    time.h
 @brief   Definition of time related data structures and functions
 @details
 @verbatim

  This header file provides the definitions for time-related data structures
  and functions used in the HanOS kernel. It includes macros for time
  conversions, type definitions for representing time and time zones, and a 
  function prototype for converting time values to a structured format.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <arch/x64/hpet.h>
#include <arch/x64/cmos.h>

#define hpet_sleep(x)       hpet_nanosleep(MILLIS_TO_NANOS(x))

#define SECONDS_TO_NANOS(x) ((x)*1000000000ULL)
#define MILLIS_TO_NANOS(x)  ((x)*1000000ULL)
#define MICROS_TO_NANOS(x)  ((x)*1000ULL)
#define NANOS_TO_SECONDS(x) ((x) / 1000000000ULL)
#define NANOS_TO_MILLIS(x)  ((x) / 1000000ULL)
#define NANOS_TO_MICROS(x)  ((x) / 1000ULL)

typedef uint64_t time_t;

/**
 * @brief Time zone offset and daylight-saving correction
 */
typedef struct {
    int32_t minuteswest;            /* minutes west of Greenwich */
    int32_t dsttime;                /* type of DST correction */
} timezone_t;

/**
 * @brief Broken-down calendar time
 */
typedef struct {
    int32_t sec;                    /* Seconds (0-60) */
    int32_t min;                    /* Minutes (0-59) */
    int32_t hour;                   /* Hours (0-23) */
    int32_t mday;                   /* Day of the month (1-31) */
    int32_t mon;                    /* Month (0-11) */
    int32_t year;                   /* Year - 1900 */
    int32_t wday;                   /* Day of the week (0-6, Sunday = 0) */
    int32_t yday;                   /* Day in the year (0-365, 1 Jan = 0) */
    int32_t isdst;                  /* Daylight saving time */
} tm_t;

void localtime(const time_t * timep, tm_t * tm);
