/**
 *  This program runs child test programs concurrently.
 */

#include "types.h"
#include "stat.h"
#include "user.h"

// Number of child programs
#define CNT_CHILD           5

// Name of child test program that tests Stride scheduler
#define NAME_CHILD_STRIDE   "test_stride"
// Name of child test program that tests MLFQ scheduler
#define NAME_CHILD_MLFQ     "test_mlfq"

char *child_argv[CNT_CHILD][3] = {
  {NAME_CHILD_MLFQ, "0", 0},
  {NAME_CHILD_MLFQ, "0", 0},
  {NAME_CHILD_MLFQ, "0", 0},
  {NAME_CHILD_MLFQ, "0", 0},
  {NAME_CHILD_STRIDE, "80", 0},
};

int
main(int argc, char *argv[])
{
  int pid;
  int i;

  for (i = 0; i < CNT_CHILD; i++) {
    pid = fork();
    if (pid > 0) {
      // parent
      continue;
    } else if (pid == 0) {
      // child
      exec(child_argv[i][0], child_argv[i]);
      printf(1, "exec failed!!\n");
      exit();
    } else {
      printf(1, "fork failed!!\n");
      exit();
    }
  }
 
  for (i = 0; i < CNT_CHILD; i++) {
    wait();
  }

  exit();
}
