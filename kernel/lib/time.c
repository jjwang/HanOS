/**-----------------------------------------------------------------------------

 @file    time.c
 @brief   Implementation of time related functions
 @details
 @verbatim

  This file provides the implementation of various time-related functions used
  in the HanOS kernel. It includes functions to determine if a year is a leap
  year, calculate the day of the week, determine the number of days in a month,
  convert time values to a structured format, and convert structured time back
  to a time value.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <lib/klog.h>
#include <lib/time.h>

static int32_t year_is_leap(int32_t year)
{
    return ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0)));
}

static int32_t day_of_week(int64_t seconds)
{
    int64_t day = seconds / 86400;
    day += 4;
    return day % 7;
}

static int64_t days_in_month(int32_t month, int32_t year)
{
    switch (month) {
    case 12:
        return 31;
    case 11:
        return 30;
    case 10:
        return 31;
    case 9:
        return 30;
    case 8:
        return 31;
    case 7:
        return 31;
    case 6:
        return 30;
    case 5:
        return 31;
    case 4:
        return 30;
    case 3:
        return 31;
    case 2:
        return year_is_leap(year) ? 29 : 28;
    case 1:
        return 31;
    }
    return 0;
}

void localtime(const time_t * timep, tm_t * _timevalue)
{
    time_t seconds = 0;
    time_t year_sec = 0;
    time_t cur_time = *timep;

    for (int32_t year = 1970; year < 2100; ++year) {
        int64_t added = year_is_leap(year) ? 366 : 365;
        int64_t secs = added * 86400;

        if (seconds + secs > cur_time) {
            _timevalue->year = year - 1900;
            year_sec = seconds;
            for (int32_t month = 1; month <= 12; ++month) {
                secs = days_in_month(month, year) * 86400;
                if (seconds + secs > cur_time) {
                    _timevalue->mon = month - 1;
                    for (int32_t day = 1; day <= days_in_month(month, year);
                         ++day) {
                        secs = 60 * 60 * 24;
                        if (seconds + secs > cur_time) {
                            _timevalue->mday = day;
                            for (int32_t hour = 1; hour <= 24; ++hour) {
                                secs = 60 * 60;
                                if (seconds + secs > cur_time) {
                                    int64_t remaining = cur_time - seconds;
                                    _timevalue->hour = hour - 1;
                                    _timevalue->min = remaining / 60;
                                    _timevalue->sec = remaining % 60;
                                    _timevalue->wday = day_of_week(*timep);
                                    _timevalue->yday =
                                        (cur_time - year_sec) / 86400;
                                    _timevalue->isdst = 0;
                                    return;
                                } else {
                                    seconds += secs;
                                }
                            }
                            return;
                        } else {
                            seconds += secs;
                        }
                    }
                    return;
                } else {
                    seconds += secs;
                }
            }
            return;
        } else {
            seconds += secs;
        }
    }
}
