#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fs.h"
#include "user.h"

void find(char *path, char *target);

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(2, "Usage: find <path> <name>\n");
    exit(1);
  }

  find(argv[1], argv[2]);
  exit(0);
}

void find(char *path, char *target) {
  char buf[512], *p;  // 用于构建新的路径
  int fd;             // 文件描述符
  struct dirent de;   // 目录项
  struct stat st;     // 文件状态

  //  打开路径
  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  // 获取文件状态
  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) {
    case T_FILE:
      // 如果是文件，检查文件名是否匹配目标名称
      p = path + strlen(path);  // 指向路径字符串的末尾

      // 找到文件名的开始位置
      while (p >= path && *p != '/') {
        p--;
      }
      p++;  // 指向字符串开头

      if (strcmp(p, target) == 0) {
        printf("%s\n", path);
      }
      break;

    case T_DIR:
      // 如果是目录，首先检查目录名是否匹配
      p = path + strlen(path);
      while (p >= path && *p != '/') {
        p--;
      }
      p++;  // 指向字符串开头

      if (strcmp(p, target) == 0) {
        printf("%s\n", path);
      }

      // 然后递归搜索目录内容
      if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        printf("find: path too long\n");
        break;
      }

      // 使用buf构建新的路径
      strcpy(buf, path);
      p = buf + strlen(buf);  // 指向路径字符串的末尾
      *p++ = '/';

      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        // 跳过空的目录项
        if (de.inum == 0) {
          continue;
        }

        // 跳过当前目录和父目录
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
          continue;
        }

        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;

        // 递归调用find
        find(buf, target);
      }
      break;
  }

  close(fd);
}