/**-----------------------------------------------------------------------------

 @file    time.c
 @brief   Implementation of time related functions
 @details
 @verbatim

  e.g., localtime...

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <lib/klog.h>
#include <lib/time.h>

static int year_is_leap(int year)
{
    return ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0)));
}

static int day_of_week(long seconds)
{
    long day = seconds / 86400;
    day += 4;
    return day % 7;
}

static long days_in_month(int month, int year)
{
    switch(month) {
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

void localtime(const time_t* timep, tm_t* _timevalue)
{
    time_t seconds = 0;
    time_t year_sec = 0;

    for (int year = 1970; year < 2100; ++year) {
        long added = year_is_leap(year) ? 366 : 365;
        long secs = added * 86400;

        if (seconds + secs > *timep) {
            _timevalue->year = year - 1900;
            year_sec = seconds;
            for (int month = 1; month <= 12; ++month) {
                secs = days_in_month(month, year) * 86400;
                if (seconds + secs > *timep) {
                    _timevalue->mon = month - 1;
                    for (int day = 1; day <= days_in_month(month, year); ++day) {
                        secs = 60 * 60 * 24;
                        if (seconds + secs > *timep) {
                            _timevalue->mday = day;
                            for (int hour = 1; hour <= 24; ++hour) {
                                secs = 60 * 60;
                                if (seconds + secs > *timep) {
                                    long remaining = *timep - seconds;
                                    _timevalue->hour = hour - 1;
                                    _timevalue->min = remaining / 60;
                                    _timevalue->sec = remaining % 60;
                                    _timevalue->wday = day_of_week(*timep);
                                    _timevalue->yday = (*timep - year_sec) / 86400;
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

time_t mktime(tm_t* tm)
{
    return
      secs_of_years(tm->year + 1900) +
      secs_of_month(tm->mon, tm->year + 1900) +
      (tm->mday - 1) * 86400 +
      (tm->hour) * 3600 +
      (tm->min) * 60 +
      (tm->sec);
}

