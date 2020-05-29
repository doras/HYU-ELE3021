#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
uint boostcnt;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  struct proc *mainthd = myproc();

  if(myproc())
    if(mainthd->tgid != mainthd->tid)
      mainthd = mainthd->parent;

  if(tf->trapno == T_SYSCALL){
    if(mainthd->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(mainthd->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
      
      boostcnt++;
      if(boostcnt >= 200){
        boostcnt = 0;
        priboost();
      }
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    mainthd->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && mainthd->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Check time quantum and time allotment.
  // If it needs yield, give up CPU. 
  if(myproc() && mainthd->state == RUNNING &&
      tf->trapno == T_IRQ0+IRQ_TIMER){

    mainthd->quancnt++;
    mainthd->alltcnt++;
    
    switch(mainthd->quelev){
    case 3:
      // It is stride process.
      if(mainthd->quancnt >= TIMEQUANTUM3){
        yield();
      }
      break;
  
    case 2:
      if(mainthd->alltcnt >= TIMEALLOT2){
        declevel(mainthd);
        yield();
      } else if(mainthd->quancnt >= TIMEQUANTUM2){
        yield();
      }
      break;
    
    case 1:
      if(mainthd->alltcnt >= TIMEALLOT1){
        declevel(mainthd);
        yield();
      } else if(mainthd->quancnt >= TIMEQUANTUM1){
        yield();
      }
      break;
    
    case 0:
      if(mainthd->quancnt >= TIMEQUANTUM0){
        yield();
      }
      break;
    }

    // If this process is multi-thread process,
    // Need to switch to another LWP.
    if(mainthd->numthd != 1){
      lwpyield();
    }
  }


  // Check if the process has been killed since we yielded
  if(myproc() && mainthd->killed && (tf->cs&3) == DPL_USER)
    exit();
}
