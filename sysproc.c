#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "syscall.h"
#include <stddef.h>
extern int clone(void(*fcn)(void *, void *), void *v1, void *v2, void *stack);
extern int join(void **stack);
extern struct spinlock memlock;
int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  
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
  acquire(&memlock);
  addr = myproc()->sz;
  if(growproc(n) < 0){
    release(&memlock);
    return -1;
  }
  release(&memlock);
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


int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int sys_clone(void)
{
  void (*fcn)(void *, void *);
  void *v1;
  void *v2;
  void *stack;
  if(argptr(0, (void*)&fcn, sizeof(void*)) < 0)
    return -1;
  if(argptr(1, (void*)&v1, sizeof(void*)) < 0)
    return -1;
  if(argptr(2, (void*)&v2, sizeof(void*)) < 0)
    return -1;
  if(argptr(3, (void*)&stack, sizeof(void*)) < 0)
    return -1;
  return clone(fcn, v1, v2, stack);
}

int sys_join(void)
{
  void **stack = NULL;
  if(argptr(0, (void*)&stack, sizeof(void**)) < 0)
    return -1;
  return join(stack);
}
