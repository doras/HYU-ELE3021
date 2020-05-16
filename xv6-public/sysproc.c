#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "thread.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getppid(void)
{
    return getppid();
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}


int
sys_yield(void)
{
  myproc()->alltcnt++;
  if((myproc()->quelev == 2 && myproc()->alltcnt >= TIMEALLOT2)
      || (myproc()->quelev == 1 && myproc()->alltcnt >= TIMEALLOT1)){
    declevel(myproc());
  }
  yield();
  return 0;
}

int
sys_getlev(void)
{
  return myproc()->quelev;
} 

int
sys_set_cpu_share(void)
{
  int shr;

  if(argint(0, &shr) < 0)
    return -1;
  return set_cpu_share(shr);
}

int
sys_thread_create(void)
{
  struct thread_t *thread;
  void* (*start_routine)(void*);
  void* arg;

  if(argptr(0, (void*)&thread, sizeof(*thread)) < 0 ||
     argint(1, (int*)&start_routine) < 0 || argint(2, (int*)&arg) < 0)
    return -1;
  return thread_create(thread, start_routine, arg);
}

int
sys_thread_exit(void)
{
  void *retval;
  
  if(argint(0, (int*)&retval) < 0)
    panic("syscall thread exit");
  thread_exit(retval);
 
  // This function never return.
  return -1;
}

int
sys_thread_join(void)
{
  struct thread_t thread;
  void **retval;

  if(argptr(0, (void*)&thread, sizeof(thread)) < 0 ||
     argint(1, (int*)&retval) < 0)
    return -1;
  return thread_join(thread, retval);
}
