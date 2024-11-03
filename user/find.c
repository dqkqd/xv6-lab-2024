#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

void
find(char *path, char* pat)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_DEVICE:
  case T_FILE:
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("find: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';

    char fullpath[512];
    memmove(fullpath, path, strlen(path));
    memmove(fullpath + strlen(path), "/\0", 2);
    int pathlen = strlen(fullpath);

    while (read(fd, &de, sizeof(de)) == sizeof(de)) {
      strcpy(fullpath + pathlen, de.name);
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("find: cannot stat %s\n", buf);
        continue;
      }
      switch (st.type) {
      case T_DEVICE:
      case T_FILE:
        if (strcmp(de.name, pat) == 0)
          printf("%s\n", fullpath);
        break;
      case T_DIR:
        if (strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0)
          find(fullpath, pat);
        break;
      }
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "Usage: find directory files...\n");
    exit(1);
  }
  find(argv[1], argv[2]);
  exit(0);
}
