#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
  int fd = open("hello", 2);
/* test for milestone1
  int num = 300;

  int buf[128];


  int i;
  int result;
  for(i = 0; i < num; i++){
    result = read(fd, buf, 512);
printf(1, "%d-th read: %d\n%d -> %d\n", i, result, buf[0], buf[127]);
  }
*/

  int num;
  int i;
  int result, result_sum = 0;
/*  for(i = 0; i < 10; i++){
    result = read(fd, &num, 4);
    printf(1, "%d-th read(%d): %x\n", i, result, num);
  }
  
  printf(1, "pread(%d): %x\n", pread(fd, &num, 4, 12), num);
*/

  for(i = 0; i < 100; i++){
    result = pread(fd,&num, 4, 8388746 + 2048*i);
    result_sum += (result != 4 || num != 0x12345678);
  }
  printf(1, "resultsum: %d\n", result_sum);

  close(fd);
  exit();
}
