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
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if ((fd = open(path, 0)) < 0) {
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if (fstat(fd, &st) < 0) {
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch (st.type) {
    case T_FILE:
      // 如果是文件，检查文件名是否匹配目标名称
      p = path + strlen(path);
      while (p >= path && *p != '/') {
        p--;
      }
      p++;  // 跳过'/'或指向字符串开头

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
      p++;  // 跳过'/'或指向字符串开头

      if (strcmp(p, target) == 0) {
        printf("%s\n", path);
      }

      // 然后递归搜索目录内容
      if (strlen(path) + 1 + DIRSIZ + 1 > sizeof buf) {
        printf("find: path too long\n");
        break;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';

      while (read(fd, &de, sizeof(de)) == sizeof(de)) {
        if (de.inum == 0) {
          continue;
        }

        // 跳过 "." 和 ".." 目录
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