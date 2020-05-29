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
  int overflow;                   // Set during overflag over the table.
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

  p->tid = p->pid;
  p->tgid = p->pid;
  p->numthd = 1;
  p->nexttid = p->pid + 1;
  p->nextlwp = p;
  p->prevlwp = p;
  p->recentlwp = p;

  p->lwpstate = L_RUNNABLE;
  p->joinproc = 0;

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
  struct proc *mainthd = myproc();

  if(curproc->tgid != curproc->tid)
    mainthd = curproc->parent;

  sz = mainthd->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
    sz = trimuvm(curproc->pgdir, sz);
  }
  mainthd->sz = sz;
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
  struct proc *mainthd;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  if(curproc->tid == curproc->tgid){
    mainthd = curproc;
  } else {
    mainthd = curproc->parent;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(mainthd->pgdir, mainthd->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = mainthd->sz;
  np->parent = mainthd;
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

  np->tid = pid;
  np->tgid = pid;
  np->numthd = 1;
  np->nexttid = pid + 1;
  np->nextlwp = np;
  np->prevlwp = np;
  np->recentlwp = np;

  np->lwpstate = L_RUNNABLE;
  np->joinproc = 0;

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

  if(curproc->tgid != curproc->tid)
    curproc = curproc->parent;

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
    if(p->parent == curproc && p->tgid != curproc->tgid){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();

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
  struct proc *mainthd = myproc();
  struct proc *lwp;
 
  if(curproc->tgid != curproc->tid)
    mainthd = curproc->parent;
 
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != mainthd || p->tgid == mainthd->tgid)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        freevm(p->pgdir);
        lwp = p;
        do{
          if(lwp->tgid != p->tgid)
            panic("wait tgid");
          kfree(lwp->kstack);
          lwp->kstack = 0;
          lwp->pid = 0;
          lwp->parent = 0;
          lwp->name[0] = 0;
          lwp->killed = 0;
          lwp->state = UNUSED;

          lwp = lwp->nextlwp;
        } while(lwp != p);

        if(p->quelev <= 2){
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
          if(sp == &sptable.proc[NPROC+1]){
            panic("wait stride process");
          }
        }
        
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || mainthd->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(mainthd, &ptable.lock);  //DOC: wait-sleep
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
  uint mlfqstride = 0;
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

      // If the process is not runnable
      // , just proceed pass and keep searching.
      if(p->state != RUNNABLE){
        goto scheduledone;
      }

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p->recentlwp;
      switchuvm(p->recentlwp);
      c->proc->lwpstate = L_RUNNING;
      p->state = RUNNING;

      swtch(&(c->scheduler), c->proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
 
      mlfqstride = minsproc->stride;

    } else {
      // If minsproc is MLFQ processes.
      ticks0 = ticks;
      mlfqstride = minsproc->stride;

      release(&ptable.lock);

highest:   
      
      // Enable interrupts on this processor.
      sti();

      acquire(&ptable.lock);

      if(ticks - ticks0 > TIMEQUANTUM3)
        goto scheduledone;

      i = startpoint2;

      do{
        if(fbqueue[2].proc[i] != 0 && 
           fbqueue[2].proc[i]->state == RUNNABLE){
          p = fbqueue[2].proc[i];
          
          // Switch to chosen process.  It is the process's job
          // to release ptable.lock and then reacquire it
          // before jumping back to us.
          c->proc = p->recentlwp;
          switchuvm(p->recentlwp);
          c->proc->lwpstate = L_RUNNING;
          p->state = RUNNING;

          swtch(&(c->scheduler), c->proc->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint2 = (i + 1) % NPROC;

          mlfqstride = minsproc->stride;

          goto scheduledone;
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
          c->proc = p->recentlwp;
          switchuvm(p->recentlwp);
          c->proc->lwpstate = L_RUNNING;
          p->state = RUNNING;

          swtch(&(c->scheduler), c->proc->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint1 = (i + 1) % NPROC;

          mlfqstride = minsproc->stride * 2;

          goto scheduledone;
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
          c->proc = p->recentlwp;
          switchuvm(p->recentlwp);
          c->proc->lwpstate = L_RUNNING;
          p->state = RUNNING;

          swtch(&(c->scheduler), c->proc->context);
          switchkvm();

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;

          // Go back to the search routine.
          startpoint0 = (i + 1) % NPROC;

          mlfqstride = minsproc->stride * 4;

          goto scheduledone;
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
        ((minsproc->pass + mlfqstride) & mask) == 0)
      sptable.overflow = 1;
    minsproc->pass += mlfqstride;

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
      switchuvm(p->recentlwp);
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
  struct proc *proc;
  acquire(&ptable.lock);  //DOC: yieldlock
  if(myproc()->tid == myproc()->tgid)
    proc = myproc();
  else
    proc = myproc()->parent;
  myproc()->lwpstate = L_RUNNABLE;
  proc->state = RUNNABLE;
  proc->recentlwp = myproc();
  sched();
  proc->quancnt = 0;
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
  struct proc *lwp;
  struct proc *mainthd = myproc();
  
  if(mainthd->tgid != mainthd->tid)
    mainthd = mainthd->parent;

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
  p->lwpstate = L_SLEEPING;

  lwp = p->nextlwp;
  while(lwp->lwpstate != L_RUNNABLE && lwp != p){
    lwp = lwp->nextlwp;
  }
  // Not found. There is no LWP that can run.
  if(lwp == p){
    mainthd->state = SLEEPING;
    mainthd->recentlwp = p;
    sched();
  }
  // Found the LWP which can run.
  else {
    lwpswtch();
  }

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
  struct proc *p, *mainthd;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->lwpstate == L_SLEEPING && p->chan == chan){
      p->lwpstate = L_RUNNABLE;
      mainthd = p;
      if(p->tgid != p->tid)
        mainthd = p->parent;
      if(mainthd->state == SLEEPING){
        mainthd->state = RUNNABLE;
        mainthd->recentlwp = p;
      }
    }
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
  struct proc *p, *lwp;
  int found = 0;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid && p->tid == p->tgid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        lwp = p;
        do{
          if(lwp->lwpstate == L_SLEEPING){
            lwp->lwpstate = L_RUNNABLE;
            p->recentlwp = lwp;
            found = 1;
            break;
          }
          lwp = lwp->nextlwp;
        } while(lwp != p);
        if(!found)
          panic("kill sleeping LWP");
      }

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
  [ZOMBIE]    "zombie",
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
  struct proc *mainthd = myproc();
  if(mainthd->tgid != mainthd->tid)
    mainthd = mainthd->parent;
  return mainthd->parent->pid;
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
  if(currproc->tgid != currproc->tid)
    currproc = currproc->parent;

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

  // quelev of stride process is 3 which is actually invalid MLFQ quelev.
  currproc->quelev = 3;

  return 0;
}

static struct proc*
alloclwp(struct proc *caller)
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
  p->pid = caller->pid;

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

int
thread_create(struct thread_t *thread, 
              void* (*start_routine)(void*), void *arg)
{
  int i;
  struct proc *np;
  struct proc *curproc = myproc();
  uint sz, sp, ustack[2];

  // Allocate LWP.
  if((np = alloclwp(curproc)) == 0){
    return -1;
  }

  np->sz = 0;
  
  // Set parent of new process to main thread.
  if(curproc->tgid == curproc->tid){
    np->parent = curproc;
  } else {
    np->parent = curproc->parent;
  }

  *np->tf = *curproc->tf;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = curproc->ofile[i];
  np->cwd = curproc->cwd;

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  np->pgdir = curproc->pgdir;

  np->quancnt = -1;
  np->alltcnt = -1;
  np->quelev = -1;

  acquire(&ptable.lock);

  np->tid = np->parent->nexttid++;
  np->tgid = curproc->pid;
  np->numthd = 0;
  np->parent->numthd++;
  np->nexttid = -1;

  np->prevlwp = np->parent;
  np->nextlwp = np->parent->nextlwp;
  np->parent->nextlwp = np;
  np->nextlwp->prevlwp = np;
  np->recentlwp = 0;

  np->state = LWP;
  np->joinproc = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = np->parent->sz;
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(np->pgdir, sz, sz + 2*PGSIZE)) == 0){
    goto bad;
  }
  clearpteu(np->pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  np->parent->sz = sz;

  ustack[0] = 0xffffffff; // fake return addr
  ustack[1] = (uint)arg;

  sp -= 2 * sizeof(uint);

  if(copyout(np->pgdir, sp, ustack, 2 * sizeof(uint)) < 0)
    goto bad;

  np->tf->eip = (uint)start_routine;
  np->tf->esp = sp;

  thread->tid = np->tid;
  thread->tgid = np->tgid;

  np->lwpstate = L_RUNNABLE;

  release(&ptable.lock);

  return 0;
bad:
  np->parent->numthd--;
  kfree(np->kstack);
  np->state = UNUSED;
  release(&ptable.lock);
  return -1;
}

// switch to another runnable LWP
void
lwpswtch(void)
{
  struct proc *curproc = myproc();
  struct proc *proc;
 
  proc = curproc->nextlwp;

  while(proc->lwpstate != L_RUNNABLE){
    proc = proc->nextlwp;
  }

  if(proc != curproc){
    mycpu()->proc = proc;
    proc->lwpstate = L_RUNNING;
    mycpu()->ts.esp0 = (uint)proc->kstack + KSTACKSIZE;
    swtch(&(curproc->context), proc->context);
  }
}

// LWP version of yield
void
lwpyield(void)
{
  struct proc *curproc = myproc();
  acquire(&ptable.lock);
  curproc->lwpstate = L_RUNNABLE;
  lwpswtch();
  release(&ptable.lock);
}

void
thread_exit(void *retval)
{
  struct proc *curproc = myproc();
  struct proc *mainthd = curproc;
  struct proc *proc;

  if(curproc->tgid != curproc->tid) {
    mainthd = curproc->parent;
  }

  curproc->cwd = 0;

  acquire(&ptable.lock);

  mainthd->numthd--;

  if(mainthd->numthd == 0){
    exit();
  }

  // A LWP might be sleeping in thread_join().
  wakeup1(curproc->joinproc);


  // Set retval
  curproc->tf->eax = (uint)retval;
  // Jump into the next LWP, never to return.
  curproc->lwpstate = L_ZOMBIE;
  
  // Search next LWP.
  proc = curproc->nextlwp;

  while(proc != curproc){
    if(proc->lwpstate == L_RUNNABLE)
      break;
    proc = proc->nextlwp;
  }

  if(proc == curproc){
    mainthd->state = SLEEPING;
    mainthd->quancnt = 0;
    sched();
  } else {
    lwpswtch();
  }
  panic("zombie thread exit");
}

int
thread_join(struct thread_t thread, void **retval)
{
  struct proc *p;
  struct proc *curproc = myproc();
  struct proc *target = 0;
  struct proc *mainthd = myproc();
  uint a, pg, pa;
  pte_t *pte;


  if(curproc->tgid != curproc->tid)
    mainthd = curproc->parent;
  
  acquire(&ptable.lock);
  // Scan through table looking for target LWP.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->tid != thread.tid || p->tgid != thread.tgid)
      continue;
    // Found one.
    target = p;
    p->joinproc = curproc;
    break;
  }
  for(;;){
    // No point waiting if we don't have such thread.
    if(!target || mainthd->killed){
      target->joinproc = 0;
      release(&ptable.lock);
      return -1;
    }
    if(target->joinproc != curproc){
      release(&ptable.lock);
      return -1;
    }

    if(target->lwpstate == L_ZOMBIE){
      if(retval)
        *retval = (void*)p->tf->eax;
      // Clean the stack.
      a = PGROUNDUP(p->tf->esp);
      for(pg = a- 2*PGSIZE; pg < a; pg += PGSIZE){
        pte = walkpgdir(target->pgdir, (char*)pg, 0);
        if(!pte)
          panic("thread join pte");
        else if((*pte & PTE_P) != 0){
          pa = PTE_ADDR(*pte);
          if(pa == 0)
            panic("thread join pa");
          kfree((char*)P2V(pa));
          *pte = 0;
        }
      }
      mainthd->sz = trimuvm(mainthd->pgdir, mainthd->sz);
      if(target != mainthd){
        // target is not main thread. just cleanup it.
        kfree(target->kstack);
        target->kstack = 0;
        target->pid = 0;
        target->parent = 0;
        target->name[0] = 0;
        target->killed = 0;
        target->state = UNUSED;
  
        target->prevlwp->nextlwp = target->nextlwp;
        target->nextlwp->prevlwp = target->prevlwp;

        release(&ptable.lock);
        return 0;
      } else {
        // target is main thread of this process.
        // we cannot cleanup it.
        kfree(target->kstack);
        target->kstack = 0;
        target->lwpstate = L_UNUSED;

        release(&ptable.lock);
        return 0;
      }
    }
    // Wait for children to exit.  (See wakeup1 call in thread_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

void
exec1(struct proc *mainthd)
{
  struct proc **queelem;
  struct sproc *strproc;

  if(mainthd->quelev <= 2){
    for(queelem = fbqueue[mainthd->quelev].proc; 
        queelem < &fbqueue[mainthd->quelev].proc[NPROC]; queelem++){
      if(*queelem == mainthd){
        *queelem = 0;
        break;
      }
    }
  } else {
    for(strproc = sptable.proc; strproc < &sptable.proc[NPROC+1]; strproc++){
      if(strproc->proc == mainthd){
        sptable.proc[0].ticket += strproc->ticket;
        sptable.proc[0].stride = 6400 / sptable.proc[0].ticket;
        strproc->proc = 0;
        strproc->ticket = 0;
        strproc->stride = 0;
        strproc->pass = 0;
        break;
      }
    }
    if(strproc == &sptable.proc[NPROC+1]){
      panic("exec stride process");
    }
  }
}

void
mlfqadd(struct proc *p)
{
  struct proc **queelem;

  acquire(&ptable.lock);
  
  for(queelem = fbqueue[2].proc; queelem < &fbqueue[2].proc[NPROC]; queelem++){
    if(*queelem == 0){
      *queelem = p;
      break;
    }
  }

  release(&ptable.lock);
}

int
sbrk(int n)
{
  int addr;
  struct proc *mainthd = myproc();
  
  if(mainthd->tgid != mainthd->tid)
    mainthd = mainthd->parent;

  acquire(&ptable.lock);
  addr = mainthd->sz;
  if(growproc(n) < 0){
    release(&ptable.lock);
    return -1;
  }
  release(&ptable.lock);
  return addr;
}

