#include "types.h"
#include "stat.h"
#include "user.h"
#include "thread.h"

#define LOOPCOUNT 100

int nums[5];

void* func(void* arg){
  int i = 0;
  int* num = arg;

  printf(1, "func!\n");

  for(; i < LOOPCOUNT; i++){
    (*num)++;
  }

  for(;;);

  return arg;
}

int main(){
  int i;

  struct thread_t thd[5];

  for(i = 0; i < 5; ++i){
    thread_create(&thd[i], &func, &nums[i]);
  }


  for(i = 0; i < 5; ++i){
    printf(1, "thread %d : %d\n", i, thd[i].tid);
  }


  for(i = 0; i < 1000000; ++i);


  for(i = 0; i < 5; ++i){
    printf(1, "%d -> %d\n", i, nums[i]);
  }

  exit();
}



