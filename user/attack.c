#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

int
main(int argc, char *argv[])
{
  // your code here.  you should write the secret to fd 2 using write
  // (e.g., write(2, secret, 8)
  char prefix[] = "my very very very secret pw is: ";
  int sz = strlen(prefix);

  while (1) {
    uint64 start = (uint64)sbrk(PGSIZE);
    if (start ==  0xffffffffffffffffLL) {
      break;
    }

    char* s = (char*)start;
    // need to leave out the first 8 bytes
    // because we used them in `kernel/kalloc.c:66`
    if (memcmp(s + 8, prefix + 8, sz - 8) == 0) {
      // get the secret
      char* secret = s + 32;

      // secret is 8 bytes length including the terminate character
      if (strlen(secret) == 7) {
        write(2, secret, 8);
        exit(0);
      }
    }
  }

  // Cannot find secret
  exit(1);
}
