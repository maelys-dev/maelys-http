#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int run_file(const char *path) {
    FILE *file = fopen(path, "rb");
    unsigned char *bytes;
    long length;
    if (!file) return 1;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }
    bytes = malloc((size_t)length + 1u);
    if (!bytes) { fclose(file); return 1; }
    if (length && fread(bytes, 1u, (size_t)length, file) != (size_t)length) {
        free(bytes); fclose(file); return 1;
    }
    fclose(file);
    (void)LLVMFuzzerTestOneInput(bytes, (size_t)length);
    free(bytes);
    return 0;
}

int main(int argc, char **argv) {
    DIR *directory;
    struct dirent *entry;
    char path[4096];
    if (argc != 2 || !(directory = opendir(argv[1]))) return 2;
    while ((entry = readdir(directory)) != NULL) {
        struct stat status;
        if (entry->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s", argv[1], entry->d_name) < 0) {
            closedir(directory); return 3;
        }
        if (stat(path, &status) == 0 && S_ISREG(status.st_mode) && run_file(path)) {
            closedir(directory); return 4;
        }
    }
    closedir(directory);
    return 0;
}
