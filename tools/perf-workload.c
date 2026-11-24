/* Fixed-volume Windows terminal throughput workload; see docs/perf.md. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 65536u
#define TOTAL (100ULL * 1024ULL * 1024ULL)

int main(int argc, char **argv)
{
    char data[BLOCK];
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE *source = NULL, *result;
    LARGE_INTEGER start, end, frequency;
    uint64_t bytes = 0;
    int mode;

    if (argc != 4)
        return 2;
    mode = strcmp(argv[1], "cat") == 0 ? 1 :
           strcmp(argv[1], "yes") == 0 ? 2 : 0;
    if (!mode)
        return 2;
    if (mode == 1) {
        source = fopen(argv[2], "rb");
        if (!source)
            return 3;
    } else {
        for (size_t i = 0; i < sizeof data; i += 2) {
            data[i] = 'y';
            data[i + 1] = '\n';
        }
    }
    /* Allow window startup and font loading before starting the timer. */
    Sleep(2000);
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    while (bytes < TOTAL) {
        size_t count = mode == 1 ? fread(data, 1, sizeof data, source) : sizeof data;
        DWORD written = 0;
        if (!count)
            break;
        for (size_t offset = 0; offset < count; offset += written) {
            if (!WriteFile(output, data + offset, (DWORD)(count - offset),
                           &written, NULL) || !written) {
                if (source)
                    fclose(source);
                return 4;
            }
        }
        bytes += count;
    }
    QueryPerformanceCounter(&end);
    if (source)
        fclose(source);
    result = fopen(argv[3], "w");
    if (!result)
        return 5;
    fprintf(result, "%llu %.9f\n", (unsigned long long)bytes,
            (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart);
    fclose(result);
    /* Keep the terminal alive briefly after saving the result. */
    Sleep(4000);
    return bytes == TOTAL ? 0 : 6;
}
