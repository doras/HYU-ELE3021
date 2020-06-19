#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
  int fd = open("hello.txt", 2);

  int num = 300;

  int buf[128];


  int i;
  int result;
  for(i = 0; i < num; i++){
    result = read(fd, buf, 512);
printf(1, "%d-th read: %d\n%d -> %d\n", i, result, buf[0], buf[127]);
  }

  close(fd);
  exit();
}
