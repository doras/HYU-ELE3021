#include "types.h"
#include "stat.h"
#include "user.h"
#include "thread.h"

#define LOOPCOUNT 100

int nums[5];

void* func(void* arg){
  int i = 0;
  int idx = *(int*)arg;
  int* num = &nums[idx];

  printf(1, "func!\n");

  for(; i < LOOPCOUNT; i++){
    (*num)++;
  }

  thread_exit(&idx);

  return 0;
}

int main(){
  int i;

  struct thread_t thd[5];

  int args[5] = {0, 1, 2, 3 ,4};


  for(i = 0; i < 5; ++i){
    thread_create(&thd[i], &func, &args[i]);
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



