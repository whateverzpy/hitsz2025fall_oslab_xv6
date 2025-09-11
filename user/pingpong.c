#include "kernel/types.h"
#include "user.h"

int main(int argc, char *argv[]) {
  int c2f[2];
  int f2c[2];
  char buf[64];
  int pid;

  if (pipe(c2f) < 0) {
    printf("pipe c2f failed\n");
    exit(1);
  }

  if (pipe(f2c) < 0) {
    printf("pipe f2c failed\n");
    exit(1);
  }

  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    int child_pid = getpid();

    close(c2f[0]);
    close(f2c[1]);

    if (read(f2c[0], buf, sizeof(buf)) <= 0) {
      printf("child: read failed\n");
      exit(1);
    }

    int father_pid = atoi(buf);

    printf("%d: received ping from pid %d\n", child_pid, father_pid);

    itoa(child_pid, buf);
    if (write(c2f[1], buf, strlen(buf) + 1) <= 0) {
      printf("child: write failed\n");
      exit(1);
    }

    close(f2c[0]);
    close(c2f[1]);

  } else {
    int father_pid = getpid();

    close(c2f[1]);
    close(f2c[0]);

    itoa(father_pid, buf);
    if (write(f2c[1], buf, strlen(buf) + 1) <= 0) {
      printf("father: write failed\n");
      exit(1);
    }

    if (read(c2f[0], buf, sizeof(buf)) <= 0) {
      printf("father: read failed\n");
      exit(1);
    }

    int child_pid = atoi(buf);

    printf("%d: received pong from pid %d\n", father_pid, child_pid);

    close(f2c[1]);
    close(c2f[0]);

    wait(0);
  }

  exit(0);
}