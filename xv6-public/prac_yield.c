#include "types.h"
#include "user.h"
#include "stat.h"

int
main(int argc, char *argv[])
{
  int pid;
  pid = fork();
  
  int i;
  for(i = 0; i < 10; ++i) { 
    if (pid != 0) {
      printf(1, "Parent\n");
      yield();
    }
    else {
      printf(1, "Child\n");
      yield();
    }
  }

  if (pid != 0) {
    wait();
  }

  exit();
}
