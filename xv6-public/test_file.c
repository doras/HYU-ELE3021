#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
/* for testing milestone 1
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
*/

  int fd = open("hello.txt", 0x202);
  int num = 0x12345678;
  printf(1, "pwrite : %d\n", pwrite(fd, &num, 4, 12));
  exit();
}
