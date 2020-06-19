#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
  int fd = open("hello.txt", 0x202);

  int num = 300;

  int buf[128];

  memset(buf, 1, 512);


  int i;
  int result;
  for(i = 0; i < num; i++){
    result = write(fd, buf, 512);
printf(1, "%d-th write: %d\n", i, result);
  }

  close(fd);
  exit();
}
