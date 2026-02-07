#include "../libsys.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        print("Usage: touch <file> [file2 ...]\n");
        exit(1);
    }

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];

        if (strlen(path) == 0) {
            print("touch: empty path\n");
            errors++;
            continue;
        }

        int result = create(path);

        if (result == 0) {  // create returns 0 on success now
            // print("touch: created '");
            // print(path);
        } else {
            print("touch: failed to create '");
            print(path);
            print("'\n");
            errors++;
        }
    }

    exit(errors);
    return 0;
}
