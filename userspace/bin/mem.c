/**-----------------------------------------------------------------------------

 @file    mem.c
 @brief   Print kernel memory totals, used and free.

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include <sysfunc.h>
#include <protocol.h>

int32_t main(int32_t argc, char *argv[])
{
    mm_stats_t st;

    (void) argc;
    (void) argv;
    if (sys_mem_stats(&st) != 0) {
        printf("mem: cannot read memory stats\n");
        return 1;
    }
    uint64_t used = (st.total > st.free) ? (st.total - st.free) : 0;

    printf("       total      used      free\n");
    printf("MiB %10lu %9lu %9lu\n",
           (unsigned long) (st.total >> 20),
           (unsigned long) (used >> 20), (unsigned long) (st.free >> 20));
    return 0;
}
