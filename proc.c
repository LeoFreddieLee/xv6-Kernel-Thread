#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
struct spinlock memlock;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
  initlock(&memlock, "memlock");
}

int cpuid()
{
  return mycpu() - cpus;
}

struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();

  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  p->thread_count = 0;
  return p;
}

void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  acquire(&ptable.lock);
  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
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
  p->tf->eip = 0;

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&ptable.lock);
}

int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  struct proc *process;
  for (process = ptable.proc; process < &ptable.proc[NPROC]; process++)
  {
    if (curproc != process && process->pgdir == curproc->pgdir)
      process->sz = sz;
  }
  switchuvm(curproc);
  return 0;
}

int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  wakeup1(curproc->parent);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

int wait(void)
{
  struct proc *p;
  int orphan, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {

    orphan = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc || curproc->pgdir == p->pgdir)
        continue;
      orphan = 1;
      if (p->state == ZOMBIE && p->thread_count == 0)
      {

        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    if (!orphan || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);
  }
}

void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {

    sti();

    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
        continue;

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

void yield(void)
{
  acquire(&ptable.lock);
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

void forkret(void)
{
  static int first = 1;

  release(&ptable.lock);

  if (first)
  {

    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }
}

void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  if (lk != &ptable.lock)
  {
    acquire(&ptable.lock);
    release(lk);
  }

  p->chan = chan;
  p->state = SLEEPING;

  sched();

  p->chan = 0;

  if (lk != &ptable.lock)
  {
    release(&ptable.lock);
    acquire(lk);
  }
}

static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
int clone(void (*fcn)(void *, void *), void *arg1, void *arg2, void *stack)
{
  int pid;
  struct proc *p2;
  struct proc *curproc = myproc();

  if (((uint)stack % PGSIZE) != 0)
    return -1;

  if ((curproc->sz - (uint)stack) < PGSIZE)
    return -1;

  if ((p2 = allocproc()) == 0)
    return -1;

  p2->pgdir = curproc->pgdir;

  int user_stack[3];
  uint stack_pointer = (uint)stack + PGSIZE;
  user_stack[0] = 0xffffffff;
  user_stack[1] = (uint)arg1;
  user_stack[2] = (uint)arg2;
  stack_pointer -= 12;
  if (copyout(p2->pgdir, stack_pointer, user_stack, 12) < 0)
    return -1;

  p2->parent = curproc;
  p2->sz = curproc->sz;
  *p2->tf = *curproc->tf;
  p2->tf->esp = (uint)stack_pointer;
  p2->tf->eax = 0;
  p2->stack = stack;
  p2->tf->eip = (uint)fcn;

  int ad = 0;
  while (ad < NOFILE)
  {
    if (curproc->ofile[ad])
      p2->ofile[ad] = filedup(curproc->ofile[ad]);

    ad++;
  }

  p2->cwd = idup(curproc->cwd);

  p2->state = RUNNABLE;
  safestrcpy(p2->name, curproc->name, sizeof(curproc->name));
  curproc->thread_count++;
  p2->thread_count = curproc->thread_count;
  pid = p2->pid;
  return pid;
}

int join(void **stack)
{
  struct proc *p;
  int orphan, pid;
  struct proc *curproc = myproc();
  acquire(&ptable.lock);
  while (1)
  {

    orphan = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc || p->pgdir != curproc->pgdir)
        continue;
      orphan = 1;

      if (p->state == ZOMBIE)
      {

        curproc->thread_count--;
        *stack = p->stack;
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->killed = 0;
        p->state = UNUSED;
        p->name[0] = 0;
        p->parent = 0;

        release(&ptable.lock);
        return pid;
      }
      else
      {
        continue;
      }
    }

    if (!orphan || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    sleep(curproc, &ptable.lock);
  }
}
