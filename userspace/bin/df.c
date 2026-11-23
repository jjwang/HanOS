/**-----------------------------------------------------------------------------

 @file    df.c
 @brief   Print disk usage of the /data ext2 mount.

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/statfs.h>

static void show(const char *path)
{
    struct statfs st;

    if (statfs(path, &st) != 0) {
        printf("%-10s (no filesystem)\n", path);
        return;
    }
    uint64_t bsize = st.f_bsize ? (uint64_t) st.f_bsize : 1;
    uint64_t total = (uint64_t) st.f_blocks * bsize;
    uint64_t avail = (uint64_t) st.f_bavail * bsize;
    uint64_t used = total - (uint64_t) st.f_bfree * bsize;

    printf("%-10s %8lu KiB total %8lu KiB used %8lu KiB avail\n",
           path, (unsigned long) (total >> 10),
           (unsigned long) (used >> 10), (unsigned long) (avail >> 10));
}

int32_t main(int32_t argc, char *argv[])
{
    (void) argc;
    (void) argv;
    show("/data");
    return 0;
}
