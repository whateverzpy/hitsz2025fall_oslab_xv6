#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags;

  if (argaddr(0, &p) < 0) return -1;

  if (argint(1, &flags) < 0) return -1;
  return wait(p, flags);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
  struct proc *p = myproc();

  // 打印当前进程内核线程上下文被保存的地址范围
  uint64 context_start = (uint64)&p->context;
  uint64 context_end = context_start + sizeof(struct context);
  printf("Save the context of the process to the memory region from address %p to %p\n", context_start, context_end);

  // 打印当前进程的pid和用户态pc值（陷入内核的指令地址）
  printf("Current running process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);

  // 模拟调度器找到下一个RUNNABLE的进程
  struct proc *next_proc = 0;
  struct proc *pp;

  // 从当前进程的下一个进程开始环形遍历
  for (pp = p + 1; pp != p; pp++) {
    // 如果到了进程表末尾，回到开头
    if (pp >= &proc[NPROC]) {
      pp = proc;
    }
    // 如果回到了当前进程，跳出循环
    if (pp == p) {
      break;
    }

    acquire(&pp->lock);
    if (pp->state == RUNNABLE) {
      next_proc = pp;
      // 打印下一个将要被调度的进程信息
      printf("Next runnable process pid is %d and user pc is %p\n", pp->pid, pp->trapframe->epc);
      release(&pp->lock);
      break;
    }
    release(&pp->lock);
  }

  // 如果没找到其他RUNNABLE进程，说明只有当前进程可运行
  if (next_proc == 0) {
    printf("Next runnable process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);
  }

  // 调用内核已实现的yield函数让出CPU
  yield();

  return 0;
}