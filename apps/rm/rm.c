#include "../libsys.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("Usage: rm <file> [file2 ...]\n");
        exit(1);
    }

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        if (strlen(path) == 0) {
            print("rm: empty path\n");
            errors++;
            continue;
        }

        int result = unlink(path);

        if (result == 0) {
            print("rm: removed '");
            print(path);
            print("'\n");
        } else {
            print("rm: failed to remove '");
            print(path);
            print("'\n");
            errors++;
        }
    }

    exit(errors);
    return 0;
}