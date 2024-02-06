#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"


void calculate_timeSlice(void);
int calculate_totalWeight(void);
int get_min_vrun_pid(void);
struct proc* get_proc(int pid);
struct proc* min_proc();
void adjust_vruntime(struct proc *p);

uint weight_list[40] = {
    88761,71755,56483,46273,36291,29154,23254,18705,14949,11916,
    9548,7620,6100,4904,3906,3121,2501,1991,1586,1277,1024,
    820,655,526,423,335,272,215,172,137,110,
    87,70,56,45,36,29,23,18,15,
};

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
  p->nice_value = 20;
  p->runtime = 0;                    // this line added
  p->weight = weight_list[p->nice_value]; // this line added
  p->vruntime = 0;
  p->carry =0;
  p->runtick = 0;
  p->startTime = ticks;
 
  for(int i = 0; i < 30; i++) {
    p->adjusted_vruntime[i] = -1; 
  }

  calculate_timeSlice();
  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    p->nice_value = 20;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at `ret,
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

  np->nice_value = curproc->nice_value;
  np->weight = curproc->weight;
  np->vruntime = curproc->vruntime;
  np->carry = curproc->carry;
  np->runtime = 0;
  np->runtick = 0;
  np->startTime = ticks;
  for (i = 0; i < 30; i++) {
    np->adjusted_vruntime[i] = curproc->adjusted_vruntime[i];
  }
  
  release(&ptable.lock); 
  calculate_timeSlice();

  //project 4
  struct mmap_area area;
  for(int i=0;i<MAX_MMAP_AREA;i++){
    struct mmap_area curr = mmap_areas[i];
	  if(curr.p == curproc){ //부모 process의 mmap_area를 찾아서 p만 바꾸고 삽입
      area.addr = curr.addr;
      area.f = curr.f;
      area.flags = curr.flags;
      area.length = curr.length;
      area.p = np;
      area.prot = curr.prot;

      mmap_areas[mmap_area_count] = area;
      mmap_area_count++;

      pte_t *pte;
      for(uint start = area.addr; start<area.addr+area.length; start+=PGSIZE){
        if((pte = walkpgdir(curproc->pgdir,(void*)start, 0))==0){
          //map populate가 아니였고, access한적이 없음. 그래서 pte를 딱히 만들 필요가 없었음 
          continue;
        }
        if(!(*pte & PTE_P)) //not present
          continue;
        uint pa = PTE_ADDR(*pte); //pte의 추출한 physical addr
        
        char* mem;
        if((mem = kalloc())==0)
          return -1;
        
        memmove(mem, (char*)P2V(pa), PGSIZE);
        if(mappages(np->pgdir, (void*)start, PGSIZE, V2P(mem), PTE_FLAGS(*pte)) < 0) {
          kfree(mem);
        }
      }
	  }
  }
  //mmap area는 생성함
  //하지만 문제는 mmap_area에 access하려고 할때
  //pte를 찾아볼건데, 이가 지금 parent 프로세스의 physical memory와
  //같이 매핑되어 있음. 따라서 우리는 child process에게
  //새로운 physical memory할당해주고 부모의 physical memory랑 copy
  
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
  
  uint curr_tick = ticks;
  curproc->runtime += (curr_tick - curproc->startTime)*1000;
  
  uint temp = curproc->vruntime;
  curproc->vruntime += (curr_tick - curproc->startTime) *1000 * weight_list[20] / curproc->weight;
  if(temp > curproc->vruntime)
    curproc->carry++;
  
  
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  calculate_timeSlice();

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


void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    // 인터럽트 활성화
    sti();

    acquire(&ptable.lock);

    

    // 최소 vruntime을 갖는 프로세스를 탐색
    struct proc *min_vruntime_proc = min_proc();

    // 최소 vruntime을 갖는 프로세스 실행
    if (min_vruntime_proc)
    {
      
      p = min_vruntime_proc;

      c->proc = p;

      switchuvm(p);
      
      calculate_timeSlice();
      p->state = RUNNING;
      p->startTime = ticks;

      swtch(&(c->scheduler), p->context);

      switchkvm();

      c->proc = 0;
    }
    
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

  uint curr_tick= ticks;

  uint temp = myproc()->vruntime;
  myproc()->runtime += (curr_tick - myproc()->startTime)*1000;
  myproc()->vruntime += (curr_tick - myproc()->startTime) *1000 * weight_list[20] / myproc()->weight;
  if(temp > myproc()->vruntime)
    myproc()->carry++;

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

  int curr_tick = ticks;
  p->runtime += (curr_tick - p->startTime)*1000;

  uint temp = p->vruntime;
  p->vruntime += (curr_tick - p->startTime)*1000 * weight_list[20] / p->weight;
  if(temp > p->vruntime)
    p->carry++;
  
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
    if (p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      
      if(min_proc() == 0){
        p->vruntime = 0;
      }
      
      if(min_proc()->vruntime < (1024000/(float)p->weight)){
        //vruntime이 음수가 되면 0으로 세팅하라
        p->vruntime = 0;
      }else{
        p->vruntime = (min_proc()->vruntime) - (1024000/p->weight);
      }
    }
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

int getpname(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      cprintf("%s\n", p->name);
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

int getnice(int pid)
{
  struct proc *p;

  if (pid <= 0)
    return -1;

  acquire(&ptable.lock);

  // iterate through the processes to find the process with given pid
  for (p = ptable.proc; p < &ptable.proc[NPROC]; ++p)
  {
    if (p->pid == pid)
    {
      release(&ptable.lock);
      return p->nice_value;
    }
  }

  release(&ptable.lock);

  // if no corresponding pid -> retun -1
  return -1;
}

int setnice(int pid, int value)
{
  struct proc *p;

  if (pid <= 0)
    return -1;

  // invalid range of nice value -> return -1
  if (value < 0 || value > 39)
    return -1;

  acquire(&ptable.lock);

  // iterate through the processes to find the process with given pid
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {

    if (p->pid == pid)
    {
      p->nice_value = value;
      p->weight = weight_list[value];
      calculate_timeSlice();
      release(&ptable.lock);
      return 0;
    }
  }

  // no corresponding pid -> return -1
  release(&ptable.lock);
  return -1;
}

void ps(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);

  if (pid == 0){    // if pid == 0, return all the processes info
    
    cprintf("%10s %10s %10s %10s %15s %10s %15s %10s %15d\n", "name", "pid", "state", "priority", "runtime/weight", "runtime", "vruntime", "tick", ticks*1000);
    
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if (p->pid != 0) //for all valid processes 
      {
        adjust_vruntime(p);
        if (p->state == EMBRYO)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "EMBRYO", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);
          
          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }

          cprintf("\n");
        }
        else if (p->state == SLEEPING)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "SLEEPING", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);
          
          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == RUNNING)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "RUNNING", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == RUNNABLE)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "RUNNABLE", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == ZOMBIE)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "ZOMBIE", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=29;i>=0;i--){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else
          continue;
      }
    }
  }

  else{
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->pid == pid)
      {
        cprintf("%10s %10s %10s %10s %15s %10s %15s %10s %15d \n", "name", "pid", "state", "priority", "runtime/weight", "runtime", "vruntime", "tick", ticks*1000);
        adjust_vruntime(p);
        if (p->state == EMBRYO)
        {
          
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "EMBRYO", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);
          
          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }

          cprintf("\n");
        }
        else if (p->state == SLEEPING)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "SLEEPING", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);
          
          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == RUNNING)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "RUNNING", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == RUNNABLE)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "RUNNABLE", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=0;i<30;i++){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
        else if (p->state == ZOMBIE)
        {
          cprintf("%10s %10d %10s %10d %15u %10u ", p->name, p->pid, "ZOMBIE", p->nice_value, ((p->runtime) / (p->weight)), p->runtime);

          for(int i=29;i>=0;i--){
            if(p->adjusted_vruntime[i]!= -1)
              cprintf("%d", p->adjusted_vruntime[i]);
          }
          cprintf("\n");
        }
      }
    }
  }
  release(&ptable.lock);
}

int calculate_totalWeight()
{
  struct proc *p;
  int total_weight = 0;

  // calculate total weight of runqueue
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
    {
      total_weight += p->weight;
    }
  }
  
  return total_weight;
}

void calculate_timeSlice()
{

  uint total_weight = calculate_totalWeight();

  struct proc *p;

  if (total_weight == 0)
  {
    return;
  }
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
    {
      p->timeSlice = 1000 *  p->weight / total_weight;
    }
  }

}

void adjust_vruntime(struct proc *p){

  uint tmp_v = p->vruntime;
  if(tmp_v==0){
    p->adjusted_vruntime[29] = 0;
  }
  for(int i=29; i>=0 && tmp_v != 0 ;i--){
    p->adjusted_vruntime[i] = tmp_v % 10;
    tmp_v /= 10; 
  }

  while(p->carry){
    uint temp = 4294967295;
    int overflow = 0;
    for(int i=29; i>=0 && temp !=0; i--){
      int sum = p->adjusted_vruntime[i] +(temp%10) + overflow;
      p->adjusted_vruntime[i] = sum % 10;
      overflow = sum / 10;
      temp /= 10;
    }
    p->carry--;
  }
}

struct proc* min_proc(){

  struct proc* p;
  // 최소 vruntime을 갖는 프로세스를 탐색
  struct proc *min_vruntime_proc = 0;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if (p->state != RUNNABLE)
      continue;

    if (!min_vruntime_proc /*아무 프로세스도 가리키지 않는경우*/|| 
        (p->vruntime < (min_vruntime_proc->vruntime) && p->carry <= (min_vruntime_proc->carry)) ||
        (p->vruntime == min_vruntime_proc->vruntime && p->carry == min_vruntime_proc->carry && p->nice_value > min_vruntime_proc->nice_value)
      )
      min_vruntime_proc = p;    
    }
  return min_vruntime_proc;
}

