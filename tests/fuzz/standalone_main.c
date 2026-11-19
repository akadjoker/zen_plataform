/*
 * standalone_main.c - runs a libFuzzer entry point on files, so the regression inputs of a fuzz target run as an
 * ordinary test with any compiler. Each argument is a file or a directory of files.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int g_runs;

static void run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    LLVMFuzzerTestOneInput(buf, got);
    free(buf);
    g_runs++;
}

static void run_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode))
    {
        run_file(path);
        return;
    }
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        run_path(sub);
    }
    if (d)
        closedir(d);
}

__attribute__((weak)) int LLVMFuzzerInitialize(int *argc, char ***argv);

int main(int argc, char **argv)
{
    if (LLVMFuzzerInitialize)
    {
        int r = LLVMFuzzerInitialize(&argc, &argv);
        if (r == 77)
        {
            printf("skip: the target needs a display\n");
            return 77;
        }
    }
    for (int i = 1; i < argc; i++)
        run_path(argv[i]);
    printf("%d inputs run\n", g_runs);
    return 0;
}
