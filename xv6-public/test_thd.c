#include "types.h"
#include "stat.h"
#include "user.h"

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

  thread_exit((void*)idx);

  return 0;
}

int main(){
  int i;

  struct thread_t thd[5];

  int retvals[5];

  int args[5] = {0, 1, 2, 3 ,4};


  for(i = 0; i < 5; ++i){
    thread_create(&thd[i], &func, &args[i]);
  }


  for(i = 0; i < 5; ++i){
    printf(1, "thread %d : %d\n", i, thd[i].tid);
  }


  for(i = 0; i < 5; ++i){
    if(thread_join(thd[i], (void**)&retvals[i]) != 0){
      printf(1, "test join fail");
      exit();
    }
  }


  for(i = 0; i < 5; ++i){
    printf(1, "%d -> %d, ret : %d\n", i, nums[i], retvals[i]);
  }

  exit();
}



