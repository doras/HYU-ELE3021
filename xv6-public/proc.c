#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct {
  struct proc *proc[NPROC];
} fbqueue[3];

struct sproc {
  struct proc *proc;  // Pointing proc element of ptable.
  int ticket;         // CPU share ratio.
  int stride;         // Stride of process.
  uint pass;          // Pass of process.
};

struct {
  struct sproc proc[NPROC+1];     // First element is always MLFQ.
  int overflow;	                        // Set during overflag over the table.
} sptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  struct proc **queelem;

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->quancnt = 0;
  p->alltcnt = 0;
  p->quelev = 2;

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  for(queelem = fbqueue[2].proc; queelem < &fbqueue[2].proc[NPROC]; queelem++){
    if(*queelem == 0){
      *queelem = p;
      break;
    }
  }

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();
  struct proc **queelem;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;


  np->quancnt = 0;
  np->alltcnt = 0;
  np->quelev = 2;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  for(queelem = fbqueue[2].proc; queelem < &fbqueue[2].proc[NPROC]; queelem++){
    if(*queelem == 0){
      *queelem = np;
      break;
    }
  }

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  struct proc **ptr;
  struct sproc *sp;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;

        if(p->quelev > 0){
          for(ptr = fbqueue[p->quelev].proc; 
              ptr < &fbqueue[p->quelev].proc[NPROC]; ptr++){
            if(*ptr == p){
              *ptr = 0;
              break;
            }
          }
        } else {
          for(sp = sptable.proc; sp < &sptable.proc[NPROC+1]; sp++){
            if(sp->proc == p){
              sptable.proc[0].ticket += sp->ticket;
              sptable.proc[0].stride = 6400 / sptable.proc[0].ticket;
              sp->proc = 0;
              sp->ticket = 0;
              sp->stride = 0;
              sp->pass = 0;
              break;
            }
          }
          // p is not a stride process.
          if(sp == &sptable.proc[NPROC+1]){
            for(ptr = fbqueue[0].proc; 
                ptr < &fbqueue[0].proc[NPROC]; ptr++){
              if(*ptr == p){
                *ptr = 0;
                break;
              }
            }
          }
        }
        
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  uint i;
  struct proc *p;
  struct sproc *sproc, *minsproc = 0;
  uint minpass;
  uint mask = 1 << (sizeof(uint) * 8 - 1);
  uint startpoint2, startpoint1, startpoint0;
  uint ticks0;
  struct cpu *c = mycpu();
  c->proc = 0;

  sptable.proc[0].proc = 0;
  sptable.proc[0].ticket = 100;
  sptable.proc[0].stride = 6400 / 100;
  sptable.proc[0].pass = 0;


  startpoint2 = 0;
  startpoint1 = 0;
  startpoint0 = 0;

  for(;;){

    // Enable interrupts on this processor.
    sti();

    acquire(&ptable.lock);

    // Find the process to run.
    // Search minimum pass.

    if(sptable.overflow){
      // If overflow is occurring, small pass values 
      // are considered an overflowed value. 
      // So, they should not be regarded as minimum pass.
      minpass = -1;
      for(sproc = sptable.proc; sproc < &sptable.proc[NPROC+1]; sproc++){
        if(sproc->proc && minpass >= sproc->pass && sproc->pass & mask){
          minpass = sproc->pass;
          minsproc = sproc;
        }
      }
    } else {
      // There is no overflow.
      minpass = sptable.proc[0].pass;
      minsproc = sptable.proc;
      for(sproc = &sptable.proc[1]; sproc < &sptable.proc[NPROC+1]; sproc++){
        if(sproc->proc && minpass > sproc->pass){
          minpass = sproc->pass;
          minsproc = sproc;
        }
      }
    }

    if(minsproc == 0)
      panic("scheduler minsproc");

    // Run the minsproc!
    

    // If the process is just stride process,
    // (not MLFQ processes)
    if(minsproc != sptable.proc){

      p = minsproc->proc;
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;


    } else {
      // If minsproc is MLFQ processes.
      ticks0 = ticks;

      release(&ptable.lock);

highest:   
      
      // Enable interrupts on this processor.
      sti();

      acquire(&ptable.lock);

      if(ticks - ticks0 > TIMEQUANTUM0)
        goto scheduledone;

      i = startpoint2;

      do{
        if(fbqueue[2].proc[i] != 0 && 
           fbqueue[2].proc[i]->state == RUNNABLE){
          p = fbqueue[2].proc[i];
          
          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;

          swtch(&(c->scheduler), p->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint2 = (i + 1) % NPROC;

          release(&ptable.lock);
          goto highest;
        }
        
        i = (i + 1) % NPROC;
      } while(i != startpoint2);

      i = startpoint1;

      do{
        if(fbqueue[1].proc[i] != 0 && 
           fbqueue[1].proc[i]->state == RUNNABLE){
          p = fbqueue[1].proc[i];
          
          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;

          swtch(&(c->scheduler), p->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint1 = (i + 1) % NPROC;

          release(&ptable.lock);
          goto highest;
        }
        
        i = (i + 1) % NPROC;
      } while(i != startpoint1);

      i = startpoint0;

      do{
        if(fbqueue[0].proc[i] != 0 && 
           fbqueue[0].proc[i]->state == RUNNABLE){
          p = fbqueue[0].proc[i];
          
          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
          c->proc = p;
          switchuvm(p);
          p->state = RUNNING;

          swtch(&(c->scheduler), p->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint0 = (i + 1) % NPROC;

          release(&ptable.lock);
          goto highest;
        }
        
        i = (i + 1) % NPROC;
      } while(i != startpoint0);

      release(&ptable.lock);
      goto highest;

    }
scheduledone:

    // Add stride to pass value.
    // And handle overflow.
    if(sptable.overflow == 0 && 
        minsproc->pass & mask && 
        ((minsproc->pass + minsproc->stride) & mask) == 0)
      sptable.overflow = 1;
    minsproc->pass += minsproc->stride;

    if(sptable.overflow){
      for(sproc = sptable.proc; sproc < &sptable.proc[NPROC+1]; sproc++){
        if(sproc->proc && sproc->pass & mask)
          break;
      }
      if(sproc >= &sptable.proc[NPROC+1])
        sptable.overflow = 0;
    }
    
    release(&ptable.lock);

  }

/*
  for(;;){

    while(fbqueue[2].numready > 0){
      // Enable interrupts on this processor.
      sti();

      acquire(&ptable.lock);
      for(p = fbqueue[2].proc; 
          p < &fbqueue[2].proc[NPROC] && fbqueue[2].numready > 0; p++){
        if(*p == 0 || (*p)->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = *p;
        switchuvm(*p);
        (*p)->state = RUNNING;

        swtch(&(c->scheduler), (*p)->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }

    while(fbqueue[2].numready < 1 && fbqueue[1].numready > 0){
      // Enable interrupts on this processor.
      sti();

      acquire(&ptable.lock);
      for(p = fbqueue[1].proc; 
          p < &fbqueue[1].proc[NPROC] 
            && fbqueue[2].numready < 1 
            && fbqueue[1].numready > 0; p++){
        if(*p == 0 || (*p)->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = *p;
        switchuvm(*p);
        (*p)->state = RUNNING;

        swtch(&(c->scheduler), (*p)->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }

    while(fbqueue[2].numready < 1 && fbqueue[1].numready < 1){
      // Enable interrupts on this processor.
      sti();

      acquire(&ptable.lock);
      for(p = fbqueue[0].proc; 
          p < &fbqueue[0].proc[NPROC] 
            && fbqueue[2].numready < 1 
            && fbqueue[1].numready < 1; p++){

        if(*p == 0 || (*p)->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = *p;
        switchuvm(*p);
        (*p)->state = RUNNING;

        swtch(&(c->scheduler), (*p)->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&ptable.lock);
    }
  }    

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }

    release(&ptable.lock);

  }
*/
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  myproc()->quancnt = 0;
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
    return myproc()->parent->pid;
}

void
declevel(struct proc *p)
{
  struct proc **procptr;
  int currlev = p->quelev;

  if(currlev < 1)
    panic("declevel quelev");

  acquire(&ptable.lock);

  for(procptr = fbqueue[currlev].proc;
      procptr < &fbqueue[currlev].proc[NPROC]; procptr++){
    if(*procptr == p)
      break;
  }

  if(procptr >= &fbqueue[currlev].proc[NPROC])
    panic("declevel p not found");
  
  *procptr = 0;

  for(procptr = fbqueue[currlev-1].proc;
      procptr < &fbqueue[currlev-1].proc[NPROC]; procptr++){
    if(*procptr == 0)
      break;
  }

  if(procptr >= &fbqueue[currlev-1].proc[NPROC])
    panic("declevel p not found");

  *procptr = p;

  p->alltcnt = 0;
  p->quelev--;

  release(&ptable.lock);
}


void
priboost(void)
{
  struct proc *tmpproc[NPROC];
  uint len = 0;
  int i;
  struct proc **procptr;

  acquire(&ptable.lock);
  
  for(procptr = fbqueue[0].proc; procptr < &fbqueue[0].proc[NPROC]; procptr++){
    if(*procptr != 0){
      tmpproc[len] = *procptr;
      *procptr = 0;
      len++;
    }
  }

  for(procptr = fbqueue[1].proc; procptr < &fbqueue[1].proc[NPROC]; procptr++){
    if(*procptr != 0){
      tmpproc[len] = *procptr;
      *procptr = 0;
      len++;
    }
  }


  for(procptr = fbqueue[2].proc, i = 0; 
      procptr < &fbqueue[2].proc[NPROC] && i < len; procptr++){
    if(*procptr == 0){
      *procptr = tmpproc[i];
      i++;
    }

    (*procptr)->alltcnt = 0;
    (*procptr)->quelev = 2;
  }

  release(&ptable.lock);
}

int
set_cpu_share(int share)
{
  struct proc **p;
  struct proc *currproc;
  int currlev;
  struct sproc *sproc;
  struct sproc *target = 0;
  uint minpass = sptable.proc[0].pass;
  uint mask = 1 << (sizeof(uint) * 8 - 1);

  // Check the total stride share is at most 80%.
  // If exceed 80%, fail and return -1.
  if(sptable.proc[0].ticket - share < 20){
    return -1;
  }

  currproc = myproc();
  currlev = currproc->quelev;
  
  // Remove from the MLFQ.
  for(p = fbqueue[currlev].proc; p < &fbqueue[currlev].proc[NPROC]; p++){
    if(*p == currproc){
      *p = 0;
      break;
    }
  }

  // Insert the process into sptable.
  if(sptable.overflow){
    minpass = (sptable.proc[0].pass & mask) ? sptable.proc[0].pass : -1;
    for(sproc = &sptable.proc[1]; sproc < &sptable.proc[NPROC+1]; sproc++){
      if(sproc->proc == 0 && !target)
        target = sproc;
      if(sproc->proc && minpass > sproc->pass && sproc->pass & mask)
        minpass = sproc->pass;
    }
  } else {
    for(sproc = &sptable.proc[1]; sproc < &sptable.proc[NPROC+1]; sproc++){
      if(sproc->proc == 0 && !target)
        target = sproc;
      if(sproc->proc && minpass > sproc->pass)
        minpass = sproc->pass;
    }
  }

  target->proc = currproc;
  target->ticket = share;
  target->stride = 6400 / share;
  target->pass = minpass;

  sptable.proc[0].ticket -= share;
  sptable.proc[0].stride = 6400 / sptable.proc[0].ticket;

  // Time quantum of stride process is same as of MLFQ lowest level.
  currproc->quelev = 0;

  return 0;
}
