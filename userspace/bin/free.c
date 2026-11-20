/**-----------------------------------------------------------------------------

 @file    free.c
 @brief   Print the kernel memory statistics

 @details
 @verbatim

  This file implements the 'free' command. It calls sys_mem_stats() and prints
  the total, used and free memory in MiB.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <sysfunc.h>
#include <protocol.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> free",     "Print total, used and free memory."},
};
/* *INDENT-ON* */

int32_t main(int32_t argc, char *argv[])
{
    mm_stats_t st;

    (void) argc;
    (void) argv;

    if (sys_mem_stats(&st) != 0) {
        printf("free: cannot read memory stats\n");
        return 1;
    }

    uint64_t used = (st.total > st.free) ? (st.total - st.free) : 0;

    printf("       total      used      free\n");
    printf("MiB %10lu %9lu %9lu\n",
           (unsigned long) (st.total >> 20),
           (unsigned long) (used >> 20), (unsigned long) (st.free >> 20));
    return 0;
}
