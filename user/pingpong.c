#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{

  int p2c[2], c2p[2];
  if(pipe(p2c) < 0){
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }
  if(pipe(c2p) < 0){
    fprintf(2, "pingpong: pipe failed\n");
    exit(1);
  }

  int pid = getpid();
  int cpid = fork();

  if (cpid != 0) {
    // parent
    close(p2c[0]);
    close(c2p[1]);

    // send a byte to the child
    if (write(p2c[1], "x", 1) < 0) {
      fprintf(2, "pingpong: parent write failed\n");
      close(p2c[1]);
      close(c2p[0]);
      exit(1);
    }
    close(p2c[1]);

    // read a byte from the child
    char buf[1];
    if (read(c2p[0], buf, 1) < 0) {
      fprintf(2, "pingpong: parent read failed\n");
      close(c2p[0]);
      exit(1);
    }
    close(c2p[0]);

    fprintf(1, "%d: received pong\n", pid);
  } else {
    // child
    close(p2c[1]);
    close(c2p[0]);

    // read a byte from parent
    char buf[1];
    if (read(p2c[0], buf, 1) < 0) {
      fprintf(2, "pingpong: children read failed\n");
      close(p2c[0]);
      close(c2p[1]);
      exit(1);
    }
    close(p2c[0]);

    fprintf(1, "%d: received ping\n", cpid);

    // write a byte to parent
    if (write(c2p[1], buf, 1) < 0) {
      fprintf(2, "pingpong: children write failed\n");
      close(c2p[1]);
      exit(1);
    }
    close(c2p[1]);
  }

  exit(0);
}
