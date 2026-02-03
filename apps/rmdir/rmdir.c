#include "../libsys.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("Usage: rmdir <directory> [directory2 ...]\n");
        exit(1);
    }

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        if (strlen(path) == 0) {
            print("rmdir: empty path\n");
            errors++;
            continue;
        }

        int result = rmdir(path);

        if (result == 0) {
            print("rmdir: removed '");
            print(path);
            print("'\n");
        } else {
            print("rmdir: failed to remove '");
            print(path);
            print("'\n");
            errors++;
        }
    }

    exit(errors);
    return 0;
}