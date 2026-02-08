#include "../libsys.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(STDOUT, " ", 1);
        print(argv[i]);
    }
    write(STDOUT, "\n", 1);
    exit(0);
    return 0;
}
