#include "../libsys.h"

int main(int argc, char **argv) {
    char buf[512];

    if (argc < 2) {
        // No arguments: read from stdin until EOF (read returns 0)
        int n;
        while ((n = read(STDIN, buf, sizeof(buf))) > 0) {
            write(STDOUT, buf, n);
        }
    } else {
        // Read each file argument
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                eprint("cat: ");
                eprint(argv[i]);
                eprint(": No such file\n");
                continue;
            }
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0) {
                write(STDOUT, buf, n);
            }
            close(fd);
        }
    }

    exit(0);
    return 0;
}
