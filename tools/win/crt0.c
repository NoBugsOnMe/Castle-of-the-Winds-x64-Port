/* minimal console start-up for the test tools (msvcrt) */
#include <stdlib.h>
int main(int, char **);
int __getmainargs(int *, char ***, char ***, int, void *);
void mainCRTStartup(void)
{
    int argc, si = 0;
    char **argv, **env;
    __getmainargs(&argc, &argv, &env, 0, &si);
    exit(main(argc, argv));
}
