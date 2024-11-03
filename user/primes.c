#include "kernel/types.h"
#include "user/user.h"

void
sendrange(int tx, int from, int to) {
  int n;
  for (n = from; n <= to; ++n) {
    if (write(tx, &n, 4) <= 0) break;
    // printf("m=%d\n", n);
  }
}

int
sendprimes(int rx, int tx, int current_prime)
{
  int succ = 0;
  int n;
  // printf("current prime: %d\n", current_prime);
  while (read(rx, &n, 4) > 0) {
    if (n % current_prime != 0) {
      if (write(tx, &n, 4) <= 0) break;
      // printf("n=%d\n", n);
      succ = 1;
    }
  }
  return succ;
}

int
recvprime(int rx) {
  int prime;
  if (read(rx, &prime, 4) <= 0) return -1;
  return prime;
}

void
consume(int rx) {
  int n;
  while (read(rx, &n, 4) > 0) {
    printf("c=%d\n", n);
  }
}

int
main(int argc, char **argv)
{
  int pp[2];
  pipe(pp);

  // first process, send all 280
  if (fork() == 0) {
    close(pp[0]);
    sendrange(pp[1], 2, 280);
    close(pp[1]);
    exit(0);
  }
  close(pp[1]);
  int source = pp[0];

  int i;
  for (i = 0; i < 280; ++i) {
    int p[2];
    pipe(p);

    int prime = recvprime(source);
    if (prime > 0)
        printf("prime %d\n", prime);

    if (fork() == 0) {
      close(p[0]);
      sendprimes(source, p[1], prime);
      close(source);
      close(p[1]);
      exit(0);
    }
    close(p[1]);
    wait(0);
    close(source);
    source = p[0];
  }

  close(source);
  wait(0);
  exit(0);
}
