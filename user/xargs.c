#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

char*
readline()
{
  static char buf[1024];
  char c;
  int i = 0;
  while (read(0, &c, 1) == 1 && c != '\n' && c != '\0')
    buf[i++] = c;
  buf[i] = '\0';
  return buf;
}

void
setargs(char **args, int at, char *arg)
{
  args[at] = malloc(strlen(arg));
  strcpy(args[at], arg);
}

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "Usage: xargs command...\n");
    exit(1);
  }

  char *args[MAXARG];
  int nargs;

  // argv[0] = xargs, so we skip it
  for (nargs = 0; nargs + 1 < argc; ++nargs)
    setargs(args, nargs, argv[nargs + 1]);

  char *line;
  while ((line = readline()) && strlen(line) > 0) {
    if (fork() == 0) {
      setargs(args, nargs, line);
      exec(args[0], args);
    }
    wait(0);
  }

  exit(0);
}
