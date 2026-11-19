#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *target = "/data/bin/c++";
    char **nv = (char **)calloc((size_t) argc + 1, sizeof(char *));

    nv[0] = (char *) target;
    for (int i = 1; i < argc; i++)
        nv[i] = argv[i];
    setenv("COMPILER_PATH", "/data/bin", 1);
    setenv("PATH", "/data/bin:/bin:/usr/bin", 1);
    execv(target, nv);
    return 127;
}
