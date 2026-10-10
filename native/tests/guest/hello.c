// Smallest end-to-end check: libc init, stdio, malloc, files, threads.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

static atomic_int counter;

static void* worker(void* arg) {
    for (int i = 0; i < 100000; ++i)
        atomic_fetch_add(&counter, 1);
    return arg;
}

int main(int argc, char** argv) {
    printf("hello from ARM64 guest, argc=%d argv[0]=%s\n", argc, argv[0]);
    char* p = malloc(1 << 20);
    memset(p, 0x5a, 1 << 20);
    printf("malloc ok: %p\n", (void*)p);
    free(p);

    FILE* f = fopen("/data/local/tmp/hello.txt", "w");
    if (f) {
        fprintf(f, "written by the guest\n");
        fclose(f);
        f = fopen("/data/local/tmp/hello.txt", "r");
        char line[64] = {0};
        fgets(line, sizeof(line), f);
        fclose(f);
        printf("file roundtrip: %s", line);
    } else {
        printf("fopen failed\n");
    }

    pthread_t th[4];
    for (int i = 0; i < 4; ++i)
        pthread_create(&th[i], NULL, worker, NULL);
    for (int i = 0; i < 4; ++i)
        pthread_join(th[i], NULL);
    printf("threads ok: counter=%d (expect 400000)\n", atomic_load(&counter));
    __android_log_print(ANDROID_LOG_INFO, "hello", "logcat works, pid %d", getpid());
    return 0;
}
