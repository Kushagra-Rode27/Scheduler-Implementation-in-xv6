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

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
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

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
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

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

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
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  // added default policy for init and sh processes
  p->sched_policy = -1;
  p->elapsed_time = 0;
  // acquire(&tickslock);
  // p->ctime = ticks;
  // // cprintf("start time of %d: %d\n",p->pid,p->ctime);
  // release(&tickslock);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
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
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
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

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  // added default policy for init and sh and other forked processes
  np->sched_policy = -1;
  np->elapsed_time = 0;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
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
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
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
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  acquire(&ptable.lock);
  struct proc *p1 = ptable.proc;
  release(&ptable.lock);

  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    struct proc *earliest = 0;
    // struct proc *p1;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE)
        continue;
      if (p->sched_policy == EDF_SCHED)
      {
        if (!earliest || p->deadline < earliest->deadline)
          earliest = p;
      }
    }
    if (earliest == 0)
    {
      for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
      {
        if (p->state != RUNNABLE)
          continue;
        if (p->sched_policy == RMS_SCHED)
        {
          if (!earliest || p->weight < earliest->weight || ((p->weight == earliest->weight) && (p->pid < earliest->pid)))
            earliest = p;
        }
      }
    }
    if (earliest == 0)
    {

      if (p1 == &ptable.proc[NPROC])
      {
        p1 = ptable.proc;
      }
      if (p1->state == RUNNABLE && p1->sched_policy != EDF_SCHED && p1->sched_policy != RMS_SCHED)
      {
        earliest = p1;
      }
      p1++;
    }
    p = earliest;
    if (p != 0)
    {
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      // cprintf("Current running process = pid %d, elapsed_time = %d\n", p->pid, p->elapsed_time);
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }

    // for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    //   if(p->state != RUNNABLE)
    //     continue;

    //   // edf policy added by us
    //   if(p->sched_policy == EDF_SCHED) {
    //     struct proc *earliest = 0;
    //     struct proc *p1;
    //     for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
    //       if(p1->state != RUNNABLE)
    //         continue;
    //       if(!earliest || p1->deadline < earliest->deadline)
    //         earliest = p1;
    //     }
    //     if(earliest != 0) {
    //       p = earliest;
    //     }
    //   }
    //   else if(p->sched_policy == RMS_SCHED) {
    //     struct proc *earliest = p;
    //     struct proc *p1 = p;
    //     for(p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++){
    //       if(p1->state != RUNNABLE)
    //         continue;
    //       if(!earliest || p1->weight < earliest->weight || ((p1->weight == earliest->weight) && (p1->pid < earliest->pid)))
    //         earliest = p1;
    //     }
    //     if(earliest != 0) {
    //       p = earliest;
    //     }
    //   }

    //   if(p != 0) {
    //     // Switch to chosen process.  It is the process's job
    //     // to release ptable.lock and then reacquire it
    //     // before jumping back to us.
    //     // cprintf("Current running process = pid %d, elapsed_time = %d\n", p->pid, p->elapsed_time);
    //     c->proc = p;
    //     switchuvm(p);
    //     p->state = RUNNING;

    //     swtch(&(c->scheduler), p->context);
    //     switchkvm();

    //     // Process is done running for now.
    //     // It should have changed its p->state before coming back.
    //     c->proc = 0;
    //   }
    // }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
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

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
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
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
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

// new functions added by us
void update_proc_stats()
{
  struct proc *p;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNING && p->sched_policy >= 0)
    {
      p->elapsed_time++;
    }
  }
  release(&ptable.lock);
  return;
}

long long int power(long long int a, long long int b)
{
  if (b == 0)
  {
    return 1;
  }
  long long int temp = power(a, b / 2);
  if (b % 2 == 0)
  {
    return temp * temp;
  }
  else
  {
    return a * temp * temp;
  }
}

int my_division(int a, int b)
{
  // // Multiply a by a large number m such that m*b > a
  // int m = 1000000;

  // // Compute the quotient using integer division
  // int q = m * a / b;

  // // Compute the fractional division part
  // int fraction = (m * a % b) * m / b;
  // int res = q + fraction;
  // return res;
  int remainder = a % b;
  int dividend = remainder * 10000;

  while (dividend / b == 0)
  {
    dividend *= 10000;
  }

  int fraction = dividend / b;

  // printf("Fractional division part of %d/%d is: %d\n", a, b, fraction);
  int res = (a / b) * 10000 + fraction;
  return res;
}

int edf_schedulable(int pid)
{
  struct proc *p;
  // int total_util = 0;
  int numerator = 0;
  int denominator = 1;

  // Calculate the total utilization of the system
  acquire(&ptable.lock);
  // for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
  //   if (p->state == RUNNABLE && p->sched_policy == EDF_SCHED) {
  //     total_util += p->exec_time/p->deadline;
  //   }
  //   else if(p->pid == pid){
  //     total_util += p->exec_time/p->deadline;
  //   }
  // }
  int count = 0;
  // acquire(&tickslock);
  // uint ticks_now = ticks;
  // release(&tickslock);

  // for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
  //   if (p->state == RUNNABLE && p->sched_policy == EDF_SCHED) {
  //     count++;
  //     denominator *= p->deadline - (ticks_now - p->ctime);
  //   } else if(p->pid == pid) {
  //     denominator *= p->deadline - (ticks_now - p->ctime);

  //   }
  // }
  // // cprintf("Count for pid %d = %d\n",pid,count);
  // for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {

  //   if (p->state == RUNNABLE && p->sched_policy == EDF_SCHED) {
  //     // cprintf("%d\n",p->deadline - (ticks_now - p->ctime));
  //     numerator += (p->exec_time - p->elapsed_time)*(denominator/(p->deadline - (ticks_now - p->ctime)));
  //   } else if(p->pid == pid) {
  //     // cprintf("%d\n",p->deadline - (ticks_now - p->ctime));
  //     numerator += (p->exec_time - p->elapsed_time)*(denominator/(p->deadline - (ticks_now - p->ctime)));
  //   }
  // }

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if ((p->state == RUNNABLE || p->state == RUNNING) && p->sched_policy == EDF_SCHED)
    {
      count++;
      // cprintf("%d sees process pid %d\n",pid,p->pid);
      denominator *= p->deadline;
    }
    else if (p->pid == pid)
    {
      count++;
      // cprintf("me here for pid %d\n",pid);
      denominator *= p->deadline;
    }
  }
  // cprintf("Count for pid %d = %d\n",pid,count);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {

    if ((p->state == RUNNABLE || p->state == RUNNING) && p->sched_policy == EDF_SCHED)
    {
      // cprintf("%d\n",p->deadline - (ticks_now - p->ctime));
      numerator += p->exec_time * (denominator / p->deadline);
    }
    else if (p->pid == pid)
    {
      // cprintf("%d\n",p->deadline - (ticks_now - p->ctime));
      numerator += p->exec_time * (denominator / p->deadline);
    }
  }

  release(&ptable.lock);
  // cprintf("PID %d: num= %d; deno= %d\n",pid, numerator, denominator);
  // Check if the utilization is less than or equal to 1
  if (numerator <= denominator)
  {
    return 0; // System is schedulable under EDF
  }
  // if (total_util <= 1) {
  //   return 0; // System is schedulable under EDF
  // }

  return -22;
}

int rms_schedulable(int pid)
{
  struct proc *p;
  // int total_util = 0;
  int numerator = 0;
  int denominator = 1;

  acquire(&ptable.lock);

  int n = 0;

  // for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
  //   // cprintf("n+=1 \n");
  //   if (p->state == RUNNABLE && p->sched_policy == RMS_SCHED) {
  //     n++;
  //     // cprintf("n+=1 \n");
  //     denominator *= p->rate;
  //   } else if(p->pid == pid) {
  //     n++;
  //     // cprintf("n+=1 \n");
  //     denominator *= p->rate;

  //   }
  // }
  // denominator*=n;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if ((p->state == RUNNABLE || p->state == RUNNING) && p->sched_policy == RMS_SCHED)
    {
      // cprintf("%d sees process pid %d\n", pid, p->pid);
      n++;
      numerator += p->exec_time * (p->rate);
    }
    else if (p->pid == pid)
    {
      // cprintf("me here for pid %d\n", pid);
      n++;
      numerator += p->exec_time * (p->rate);
    }
  }
  denominator *= 100;
  // denominator *= n * 100;
  // cprintf("Count for pid %d = %d\n", pid, n);
  // numerator += denominator;

  // cprintf("Before Num = %d, Deno = %d\n", numerator, denominator);

  // int frac = my_division(numerator, denominator);
  // numerator = power(numerator, n);
  // denominator = power(denominator, n);
  // cprintf("Num = %d, 2*Deno = %d\n", numerator, 2 * denominator);
  // cprintf("Frac = %d\n", frac);
  // cprintf("Frac^n = %d\n", power(frac / 100, n));
  // long long int final_val = power(frac / 100, n);
  int bound_vals[] = {1000, 828, 779, 756, 743, 734, 728, 724, 720, 717, \
                      715, 713, 711, 710, 709, 708, 707, 706, 705, 705, \
                      704, 704, 703, 703, 702, 702, 702, 701, 701, 701};

  release(&ptable.lock);

  if (numerator*1000 <= bound_vals[n-1] * denominator)
  // if (final_val <= 2 * power(100, n))
  // if (numerator <= 2 * denominator)
  {
    return 0; // System is schedulable under EDF
  }

  return -22;
}

int sched_policy_helper(void)
{
  // static char *states[] = {
  // [UNUSED]    "unused",
  // [EMBRYO]    "embryo",
  // [SLEEPING]  "sleep ",
  // [RUNNABLE]  "runble",
  // [RUNNING]   "run   ",
  // [ZOMBIE]    "zombie"
  // };

  int pid, policy;
  if (argint(0, &pid) < 0 || argint(1, &policy) < 0)
  {
    return -22;
  }
  int ok = 0;
  if (policy == EDF_SCHED)
  {
    int sched_result = edf_schedulable(pid);
    if (sched_result == -22)
    {
      // cprintf("PID %d killed\n", pid);
      kill(pid);
    }
    ok = sched_result;
    // return sched_result;
  }
  else if (policy == RMS_SCHED)
  {
    int sched_result = rms_schedulable(pid);
    if (sched_result == -22)
    {
      // cprintf("PID %d killed\n", pid);
      kill(pid);
    }
    ok = sched_result;
    // return sched_result;
  }

  if (ok == 0)
  {
    // cprintf("%d\n",ok);
    struct proc *p;
    int f = 0;
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->pid == pid)
      {
        f = 1;
        p->sched_policy = policy;
        acquire(&tickslock);
        p->arrival_time = ticks;
        // cprintf("start time of %d: %d\n",pid,np->ctime);
        release(&tickslock);
        // cprintf("PID %d State -> %s\n", p->pid, states[p->state]);
        break;
      }
    }
    release(&ptable.lock);

    if (!f)
      return -22;
  }

  return ok;

  // return 0;
}

int exec_time_helper(void)
{
  int pid, etime;
  if (argint(0, &pid) < 0 || argint(1, &etime) < 0)
  {
    return -22;
  }
  struct proc *p;
  int f = 0;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      f = 1;
      p->exec_time = etime;
      break;
    }
  }
  release(&ptable.lock);

  if (!f)
    return -22;

  return 0;
}

int deadline_helper(void)
{
  int pid, deadline;
  if (argint(0, &pid) < 0 || argint(1, &deadline) < 0)
  {
    return -22;
  }
  struct proc *p;
  int f = 0;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      f = 1;
      p->deadline = deadline;
      break;
    }
  }
  release(&ptable.lock);

  if (!f)
    return -22;

  return 0;
}

int rate_helper(void)
{
  int pid, rate;
  if (argint(0, &pid) < 0 || argint(1, &rate) < 0)
  {
    return -22;
  }
  struct proc *p;
  int f = 0;
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      f = 1;
      p->rate = rate;
      int prio;
      if (((30 - rate) * 3) % 29 == 0)
      {
        prio = ((30 - rate) * 3) / 29;
      }
      else
      {
        prio = (((30 - rate) * 3) / 29) + 1;
      }
      p->weight = MAXEL(1, prio);
      // cprintf("weight for pid %d = %d\n", p->pid, p->weight);
      break;
    }
  }
  release(&ptable.lock);

  if (!f)
    return -22;

  return 0;
}
