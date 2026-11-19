#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *target = "/data/bin/g++";
    char **nv = (char **)calloc((size_t) argc + 2, sizeof(char *));

    nv[0] = (char *) target;
    nv[1] = "-Wl,-z,noexecstack";
    for (int i = 1; i < argc; i++)
        nv[i + 1] = argv[i];
    setenv("COMPILER_PATH", "/data/bin", 1);
    setenv("PATH", "/data/bin:/bin:/usr/bin", 1);
    execv(target, nv);
    return 127;
}
