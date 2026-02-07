// mkdir - create directories
#include "../libsys.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("Usage: mkdir <directory> [directory2 ...]\n");
        exit(1);
    }

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        if (strlen(path) == 0) {
            print("mkdir: empty path\n");
            errors++;
            continue;
        }

        int result = mkdir(path);

        if (result == 0) {
            // print("mkdir: created '");
            // print(path);
            // print("'\n");
        } else {
            print("mkdir: failed to create '");
            print(path);
            print("'\n");
            errors++;
        }
    }

    exit(errors);
    return 0;
}
