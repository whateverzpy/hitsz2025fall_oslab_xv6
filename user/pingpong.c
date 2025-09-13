#include "kernel/types.h"
#include "user.h"

int main(int argc, char *argv[]) {
  int c2f[2];    // 子进程到父进程的管道
  int f2c[2];    // 父进程到子进程的管道
  char buf[64];  // 用于数据传递的缓冲区
  int pid;

  // 创建子到父的管道
  if (pipe(c2f) < 0) {
    printf("pipe c2f failed\n");
    exit(1);
  }

  // 创建父到子的管道
  if (pipe(f2c) < 0) {
    printf("pipe f2c failed\n");
    exit(1);
  }

  // 创建子进程
  pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }

  // 子进程逻辑
  if (pid == 0) {
    int child_pid = getpid();  // 获取子进程pid

    close(c2f[0]);  // 关闭子进程不需要的c2f读端
    close(f2c[1]);  // 关闭子进程不需要的f2c写端

    // 从父进程读取数据（父进程pid）
    if (read(f2c[0], buf, sizeof(buf)) <= 0) {
      printf("child read failed\n");
      exit(1);
    }

    int father_pid = atoi(buf);  // 将父进程pid字符串转为整数

    // 打印收到ping消息
    printf("%d: received ping from pid %d\n", child_pid, father_pid);

    itoa(child_pid, buf);  // 将子进程pid转为字符串
    // 将子进程pid写回父进程
    if (write(c2f[1], buf, strlen(buf) + 1) <= 0) {
      printf("child write failed\n");
      exit(1);
    }

    close(f2c[0]);  // 关闭f2c读端
    close(c2f[1]);  // 关闭c2f写端

  } else {                      // 父进程逻辑
    int father_pid = getpid();  // 获取父进程pid

    close(c2f[1]);  // 关闭父进程不需要的c2f写端
    close(f2c[0]);  // 关闭父进程不需要的f2c读端

    itoa(father_pid, buf);  // 将父进程pid转为字符串

    // 将父进程pid写给子进程
    if (write(f2c[1], buf, strlen(buf) + 1) <= 0) {
      printf("father write failed\n");
      exit(1);
    }

    // 从子进程读取数据（子进程pid）
    if (read(c2f[0], buf, sizeof(buf)) <= 0) {
      printf("father read failed\n");
      exit(1);
    }

    int child_pid = atoi(buf);  // 将子进程pid字符串转为整数

    // 打印收到pong消息
    printf("%d: received pong from pid %d\n", father_pid, child_pid);

    close(f2c[1]);  // 关闭f2c写端
    close(c2f[0]);  // 关闭c2f读端

    wait(0);  // 等待子进程结束
  }

  exit(0);  // 正常退出
}