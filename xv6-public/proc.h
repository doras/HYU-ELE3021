// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE, LWP };
enum lwpstate { L_UNUSED, L_SLEEPING, L_RUNNABLE, L_RUNNING, L_ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state (only valid in main thread)
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  uint quancnt;                // Elapsed time in current time quantum (ticks)
  uint alltcnt;                // Total time used in current queue (ticks)
  int quelev;                  // Current queue level

  int tid;                     // Thread ID
  int tgid;                    // Thread Group ID which is same value as pid
  int numthd;                  // The number of Thread within the process 
                               //     except UNUSED, ZOMBIE LWP.
  int nexttid;                 // Next Thread ID for thread creation

  // LWP Group is managed by doubly circular linked list.
  struct proc *nextlwp;        // Next LWP pointer
  struct proc *prevlwp;        // Previous LWP pointer

  struct proc *recentlwp;      // Most recently executed LWP

  enum lwpstate lwpstate;      // State of LWP.
  struct proc *joinproc;       // The process that call thread_join to this.
};


// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
