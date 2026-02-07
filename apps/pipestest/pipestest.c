#include "../libsys.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fds[2];

    // Create a pipe
    if (pipe(fds) < 0) {
        print("pipe() failed!\n");
        exit(1);
    }

    print("Pipe created: read_fd=");
    // TODO(human): Print fds[0] and fds[1] as numbers
    // For now, just continue with the test

    // Write to the pipe
    char *msg = "Hello from pipe!";
    int written = write(fds[1], msg, 16);

    if (written < 0) {
        print("write() failed!\n");
        exit(1);
    }

    print("Wrote to pipe\n");

    // Read from the pipe
    char buf[64];
    int bytes = read(fds[0], buf, 64);

    if (bytes < 0) {
        print("read() failed!\n");
        exit(1);
    }

    buf[bytes] = '\0';  // null-terminate

    print("Read from pipe: ");
    print(buf);
    print("\n");

    // Clean up
    close(fds[0]);
    close(fds[1]);

    print("Pipe test PASSED!\n");
    exit(0);
    return 0;
}