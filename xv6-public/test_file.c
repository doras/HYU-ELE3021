#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
// for testing milestone 1
  int fd;

//  int num = 3000;

  int buf = 0x12345678;

  int result_sum = 0;

/*
  int i, j;
  int result;
  struct stat stat;  
*/

  int result, i;
/*
  for(j = 0; j < num; j++){
  fd = open("hello", 0x202);
  result_sum = 0;
  for(i = 0; i < 100; i++){
    result = pwrite(fd,&buf, 4, 8388746 + 2048*i);
    result_sum += (result != 4);
  }
  sync();
  fstat(fd, &stat);
  printf(1, "%d-th file result : %d, size : %d\n", j, result_sum, (int)stat.size);
  close(fd);
  unlink("hello");
  }
*/

  fd = open("hello", 0x202);
  for(i = 0; i < 100; i++){
    result = pwrite(fd,&buf, 4, 8388746 + 2048*i);
    result_sum += (result != 4);
  }
  printf(1, "resultsum : %d\n", result_sum);
  close(fd);
  exit();
  
/* for testing milestone2
  int fd = open("hello.txt", 0x202);
  int num = 0x12345678;
  printf(1, "pwrite : %d\n", pwrite(fd, &num, 4, 12));
  close(fd);
*/
/*
  int fd = open("hello.txt", 0x202);
  int num = 0x12345678;
  int cnt = 15;
  int i;
  for(i = 0; i < cnt; i++){
    printf(1, "lognum: %d before %d-th pwrite\n", get_log_num(), i);
    pwrite(fd,&num, 4, 71000 + 512*i);
  }
  sync();
  for(;;);
  close(fd);
*/
  exit();
}
