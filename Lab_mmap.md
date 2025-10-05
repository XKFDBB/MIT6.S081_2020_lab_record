先为`struct proc`增加一个长度为16的VMA数组

```c++
// kernel/proc.h
#define NVMA 16

struct VMA {
  uint64 addr;
  uint64 length;
  int prot;
  int flags;
  int off;
  int valid;
  struct file *file;
};

// Per-process state
struct proc {
  struct spinlock lock;

  // ...
  char name[16];               // Process name (debugging)
  struct VMA VMA_array[NVMA];    // VMA 数组
};
```

下面实现`sys_mmap`系统调用，在这里不需要为虚拟内存分配物理页，只需要初始化一个`VMA`即可。

```c++
uint64
sys_mmap(void)
{
  uint64 addr, length, va;
  int prot, flags, fd, offset;
  struct file *f;
  if (argaddr(0, &addr) < 0 || argaddr(1, &length) < 0 || argint(2, &prot) < 0 || 
      argint(3, &flags) < 0 || argfd(4, &fd, &f) < 0 || argint(5, &offset) < 0) {
    return 0xffffffffffffffff;
  }
  if (!f)
    return 0xffffffffffffffff;
  if (addr != 0 || offset != 0)
    return 0xffffffffffffffff;
  if (!f->readable && (prot & PROT_READ))
    return 0xffffffffffffffff;
  if (!f->writable && (prot & PROT_WRITE) && (flags == MAP_SHARED))
    return 0xffffffffffffffff;
  
  struct proc* p = myproc();


  // 对齐长度
  uint64 len = PGROUNDUP(length);

  // 初始化 mmap_start
  if (p->mmap_start == 0) {
    p->mmap_start = PGROUNDUP(p->sz);
  }

  // 检查区域是否超出上限
  va = p->mmap_start;
  uint64 max_addr = TRAPFRAME - 2 * PGSIZE;
  if (va + len > max_addr) {
    return 0xffffffffffffffff;
  }

  // 检查该区域是否真的空闲（没有bug的话应该是不需要的）
  for (uint64 a = va; a < va + len; a+=PGSIZE) {
    if (walkaddr(p->pagetable, a) != 0) {
      return 0xffffffffffffffff;
    }
  }

  // 找到一个空的vma并初始化
  struct VMA* vma = 0;
  for (int i = 0; i < NVMA; ++i) {
    if (p->VMA_array[i].valid == 0) {
      vma = &p->VMA_array[i];
      break;
    }
  }
  if (vma == 0) {
    return 0xffffffffffffffff;
  }
  vma->addr = va;
  vma->prot = prot;
  vma->flags = flags;
  vma->length = length;
  vma->valid = 1;
  vma->file = f;
  vma->off = offset;
  filedup(f);
  p->mmap_start = va + len;
  return va;
}
```

在访问`mmap`返回的地址时会发生缺页中断，在`usertrap`中处理缺页中断。

```c++
void
usertrap(void)
{
  int which_dev = 0;

  // ...省略
  } else if (r_scause() == 13 || r_scause() == 15) {
    uint64 va = r_stval();
    if (handle_mmap_page_fault(va) == -1) {
      printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
      printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
      p->killed = 1;
    }
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}
```

在这里利用一个`handle_mmap_page_fault`函数处理缺页中断。

```c++
// 处理缺页中断
int
handle_mmap_page_fault(uint64 va)
{
  struct proc *p = myproc();
  struct VMA *vma = 0;

  // 查找 va 是否属于某个 mmap 区域
  for (int i = 0; i < NVMA; i++) {
    if (p->VMA_array[i].valid && va >= p->VMA_array[i].addr && va < p->VMA_array[i].addr + p->VMA_array[i].length) {
      vma = &p->VMA_array[i];
      break;
    }
  }
  if (!vma) {
    return -1;
  }

  // 对齐到页边界
  uint64 pg_va = PGROUNDDOWN(va);

  // 检查是否已映射（防止重复映射）
  if (walkaddr(p->pagetable, pg_va) != 0) {
    return -1;
  }

  // 分配物理页
  char* mem = kalloc();
  if (!mem)
    return -1;
  memset((void *)mem, 0, PGSIZE);

  // 从文件读取数据
  struct file *f = vma->file;
  if (f == 0) {
    return -1;
  }
  uint64 file_offset = (pg_va - vma->addr) + vma->off;
  ilock(f->ip);
  int n = readi(f->ip, 0, (uint64)mem, file_offset, PGSIZE);
  if (n < 0) {
    iunlock(f->ip);
    return -1;
  }
  iunlock(f->ip);

  // 映射到用户页表
  int perm = PTE_U;
  if (vma->prot & PROT_READ) {
    perm |= PTE_R;
  }
  if (vma->prot & PROT_WRITE)
    perm |= PTE_W;
  if (vma->prot & PROT_EXEC)
    perm |= PTE_X;

  if (mappages(p->pagetable, pg_va, PGSIZE, (uint64)mem, perm) < 0) {
    kfree(mem);
    return -1;
  }
  return 0;
}
```

下面实现`munmap`。

```c++
uint64
sys_munmap(void)
{
  uint64 addr, length;

  if (argaddr(0, &addr) < 0 || argaddr(1, &length) < 0)
    return -1;

  struct proc *p = myproc();
  struct VMA *vma = 0;
  
  // 找到对应地址范围的 VMA
  for (int i = 0; i < NVMA; ++i) {
    if (p->VMA_array[i].valid && addr >= p->VMA_array[i].addr && addr <= p->VMA_array[i].addr + p->VMA_array[i].length) {
        vma = &p->VMA_array[i];
        break;
    }
  }
  if (vma <= 0) {
    return -1;
  }

  // 如果页面被修改过且映射类型为 MAP_SHARED，则需将页面内容写回文件
  addr = PGROUNDDOWN(addr);
  length = PGROUNDUP(length);
  if(vma->flags & MAP_SHARED) {
    filewrite(vma->file, addr, length);
  }
  // 使用 uvmunmap 解除指定页的映射并释放物理内存
  uvmunmap(p->pagetable, addr, length/PGSIZE, 1);
  // 如果 munmap 移除了整个 mmap 区域，则需减少对应 struct file 的引用计数
  if(addr == vma->addr && length == vma->length) {
    fileclose(vma->file);
    vma->valid = 0;
  } else if (addr == vma->addr) {
    vma->addr += length;
    vma->length -= length;
    vma->off += length;
  } else if (addr + length == vma->addr + vma->length) {
    vma->length -= length;
  }

  return 0;
}
```

修改`exit`和`fork`两个函数。

```c++
// kernel/proc.c

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");
  
  for (int i = 0; i < NVMA; ++i) {
    struct VMA *vma = &p->VMA_array[i];
    if (vma->valid) {
      if (vma->flags & MAP_SHARED) {
        filewrite(vma->file, vma->addr, vma->length);
      }
      uvmunmap(p->pagetable, vma->addr, vma->length/PGSIZE, 1);
      vma->valid = 0;
    }
  }

  // ...省略
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // ...省略
  
  // 复制VMA
  for (i = 0; i < NVMA; ++i) {
    np->VMA_array[i].valid = 0;
    if (p->VMA_array[i].valid == 1) {
      memmove(&np->VMA_array[i], &p->VMA_array[i], sizeof(struct VMA));
      filedup(p->VMA_array[i].file); // 增加引用次数
    }
  }

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}
```

在测试的时候`test not-mapped unmap`会报错

```c++
test not-mapped unmap
panic: uvmunmap: not mapped
```

原因是在这个测试中`mmap`了三个页

```c++
// 158行 
p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
```

但是却只对前两个页进行了访问，因此只有前两个页存在物理地址的映射

```c++
// 168
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';
```

最后却要尝试对第三个页解除映射，这肯定会报错了。在这里简单的对`uvmunmap`进行修改，要处理没有映射的页时直接跳过。

```c++
// kernel/vm.c
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}
```

