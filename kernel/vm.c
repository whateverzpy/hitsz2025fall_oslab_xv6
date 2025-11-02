#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[];  // trampoline.S

void freewalk(pagetable_t pagetable);

/*
 * create a direct-map page table for the kernel.
 */
void kvminit() {
  kernel_pagetable = (pagetable_t)kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// 为进程创建独立的内核页表（不包含CLINT映射）
pagetable_t proc_kvminit() {
  pagetable_t kpgtbl = (pagetable_t)kalloc();
  if (kpgtbl == 0) return 0;
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  if (mappages(kpgtbl, UART0, PGSIZE, UART0, PTE_R | PTE_W) != 0) goto err;

  // virtio mmio disk interface
  if (mappages(kpgtbl, VIRTIO0, PGSIZE, VIRTIO0, PTE_R | PTE_W) != 0) goto err;

  // 注意：不映射CLINT，避免地址冲突

  // PLIC
  if (mappages(kpgtbl, PLIC, 0x400000, PLIC, PTE_R | PTE_W) != 0) goto err;

  // kernel text executable and read-only
  if (mappages(kpgtbl, KERNBASE, (uint64)etext - KERNBASE, KERNBASE, PTE_R | PTE_X) != 0) goto err;

  // kernel data and physical RAM
  if (mappages(kpgtbl, (uint64)etext, PHYSTOP - (uint64)etext, (uint64)etext, PTE_R | PTE_W) != 0) goto err;

  // trampoline
  if (mappages(kpgtbl, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) != 0) goto err;

  return kpgtbl;

err:
  // 释放页表但不释放物理页
  freewalk(kpgtbl);
  return 0;
}

// 释放进程的内核页表，但不释放物理页，且避免释放共享的用户L0页表
void proc_freekpagetable(pagetable_t kpgtbl) {
  int user_l1_lim = PLIC >> PXSHIFT(1);  // PLIC 覆盖到的 L1 项数（2MB 粒度）

  for (int i = 0; i < 512; i++) {
    pte_t pte = kpgtbl[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // 中间层页表
      uint64 child = PTE2PA(pte);
      if (i == PX(2, 0)) {
        // L2[0] -> L1：前 user_l1_lim 项共享用户的 L0，不能递归释放
        pagetable_t kl1 = (pagetable_t)child;

        // 清理 [0, user_l1_lim) 区间的 PTE 引用（不递归）
        for (int j = 0; j < user_l1_lim; j++) kl1[j] = 0;

        // 对剩余项正常递归释放（这些属于纯内核映射，如 PLIC 及以上）
        for (int j = user_l1_lim; j < 512; j++) {
          pte_t pte1 = kl1[j];
          if ((pte1 & PTE_V) && (pte1 & (PTE_R | PTE_W | PTE_X)) == 0) {
            proc_freekpagetable((pagetable_t)PTE2PA(pte1));
            kl1[j] = 0;
          } else if (pte1 & PTE_V) {
            // 叶子：只清掉映射
            kl1[j] = 0;
          }
        }
        kfree((void *)kl1);
        kpgtbl[i] = 0;
      } else {
        // 其他中间节点：递归释放
        proc_freekpagetable((pagetable_t)child);
        kpgtbl[i] = 0;
      }
    } else if (pte & PTE_V) {
      // 叶子节点：只清除PTE，不释放物理页
      kpgtbl[i] = 0;
    }
  }
  // 最后释放页表本身
  kfree((void *)kpgtbl);
}

// 将用户页表 [0, PLIC) 的 L1 目录项同步到内核页表，指向相同的 L0（共享叶子页表）
int sync_pagetable(pagetable_t pagetable, pagetable_t kpagetable) {
  // 计算用户空间在 L1 的覆盖项数（每项 2MB）
  int user_l1_lim = PLIC >> PXSHIFT(1);  // 0x0c000000 >> 21 = 96

  // 确保内核 L2[0] 存在且为中间节点
  pte_t *k_l2e = &kpagetable[PX(2, 0)];
  pagetable_t k_l1;
  if ((*k_l2e & PTE_V) == 0 || (*k_l2e & (PTE_R | PTE_W | PTE_X)) != 0) {
    k_l1 = (pagetable_t)kalloc();
    if (k_l1 == 0) return -1;
    memset(k_l1, 0, PGSIZE);
    *k_l2e = PA2PTE(k_l1) | PTE_V;  // 中间节点
  } else {
    k_l1 = (pagetable_t)PTE2PA(*k_l2e);
  }

  // 读取用户 L2[0] -> L1
  pte_t u_l2e = pagetable[PX(2, 0)];
  pagetable_t u_l1 = 0;
  if (u_l2e & PTE_V) {
    // 只能是中间节点
    u_l1 = (pagetable_t)PTE2PA(u_l2e);
  }

  // 同步前 user_l1_lim 项：将内核 L1 的对应项复制为用户 L1 的项（共享到相同 L0）
  for (int j = 0; j < user_l1_lim; j++) {
    if (u_l1) {
      k_l1[j] = u_l1[j];
    } else {
      k_l1[j] = 0;
    }
  }

  // 刷新 TLB，使得后续访问可见
  sfence_vma();
  return 0;
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void kvminithart() {
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA) panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0) return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64 walkaddr(pagetable_t pagetable, uint64 va) {
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA) return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0) return 0;
  if ((*pte & PTE_V) == 0) return 0;
  if ((*pte & PTE_U) == 0) return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void kvmmap(uint64 va, uint64 pa, uint64 sz, int perm) {
  if (mappages(kernel_pagetable, va, sz, pa, perm) != 0) panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64 kvmpa(uint64 va) {
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernel_pagetable, va, 0);
  if (pte == 0) panic("kvmpa");
  if ((*pte & PTE_V) == 0) panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm) {
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0) return -1;
    if (*pte & PTE_V) panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last) break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free) {
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0) panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0) panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V) panic("uvmunmap: not a leaf");
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t uvmcreate() {
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0) return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz) {
  char *mem;

  if (sz >= PGSIZE) panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64 uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  char *mem;
  uint64 a;

  if (newsz < oldsz) return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64 uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz) {
  if (newsz >= oldsz) return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable) {
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz) {
  if (sz > 0) uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz) {
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0) panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0) panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0) goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void uvmclear(pagetable_t pagetable, uint64 va) {
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0) panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len) {
  uint64 n, va0, pa0;

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len) n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len) {
  uint64 s = r_sstatus();
  w_sstatus(s | SSTATUS_SUM);
  int r = copyin_new(pagetable, dst, srcva, len);
  w_sstatus(s);
  return r;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max) {
  uint64 s = r_sstatus();
  w_sstatus(s | SSTATUS_SUM);
  int r = copyinstr_new(pagetable, dst, srcva, max);
  w_sstatus(s);
  return r;
}

// 递归打印页表项
// pagetable: 当前页表的物理地址
// level: 当前页表的层级 (2, 1, 0)
// va_prefix: 上层构建的虚拟地址前缀
void vmprint_recursive(pagetable_t pagetable, int level, uint64 va_prefix) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];

    // 只打印有效的PTE
    if (pte & PTE_V) {
      // 打印缩进
      for (int l = 2; l > level; l--) {
        printf("||   ");
      }
      printf("||");

      // 检查PTE是否为叶子节点
      if ((pte & (PTE_R | PTE_W | PTE_X)) != 0) {
        // 叶子节点
        uint64 va = va_prefix | ((uint64)i << (12 + 9 * level));
        printf("idx: %d: va: %p -> pa: %p, flags: %s%s%s%s\n", i, va, PTE2PA(pte), (pte & PTE_R) ? "r" : "-",
               (pte & PTE_W) ? "w" : "-", (pte & PTE_X) ? "x" : "-", (pte & PTE_U) ? "u" : "-");
      } else {
        // 中间节点 (指向下一级页表)
        printf("idx: %d: pa: %p, flags: ----\n", i, PTE2PA(pte));

        // 递归进入下一层
        uint64 next_va_prefix = va_prefix | ((uint64)i << (12 + 9 * level));
        pagetable_t child_pgtbl = (pagetable_t)PTE2PA(pte);
        vmprint_recursive(child_pgtbl, level - 1, next_va_prefix);
      }
    }
  }
}

void vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  // 从最高层(level 2)开始递归，虚拟地址前缀为0
  vmprint_recursive(pagetable, 2, 0);
}

// check if use global kpgtbl or not
int test_pagetable() {
  uint64 satp = r_satp();
  uint64 gsatp = MAKE_SATP(kernel_pagetable);
  printf("test_pagetable: %d\n", satp != gsatp);
  return satp != gsatp;
}