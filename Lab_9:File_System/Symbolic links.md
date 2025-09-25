# Symbolic links

## 编译user/symlinktest.c

根据问题提示，首先创建系统调用号

```c++
// syscall.h
#define SYS_symlink 22
```

添加一个entry

```c++
// user/usys.pl
...
entry("sbrk");
entry("sleep");
entry("uptime");
// 在后面添加entry
entry("symlink");
```

```c++
// user/user.h
...
// system calls
...
int sleep(int);
int uptime(void);
int symlink(char *, char *);
```

定义一个空的系统调用函数

```c++
// kernel/sysfile.c
...
uint64
sys_symlink(void)
{
  return 0;
}

```

```c++
// kernel/syscall.c
...
extern uint64 sys_write(void);
extern uint64 sys_uptime(void);
extern uint64 sys_symlink(void);

...
static uint64 (*syscalls[])(void) = {
...
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_symlink] sys_symlink,
};
```

添加一个新的文件类型

```c++
// kernel/stat.h
#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device
#define T_SYMLINK 4   // symlink
```

添加一个新的flag

```c++
// kernel/fcntl.h
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
#define O_NOFOLLOW 0x800
```

在makefile中添加symlinktest

```c++
ifeq ($(LAB),fs)
UPROGS += \
	$U/_symlinktest
endif
```

现在user/symlinktest.c可以正常编译了

## 实现sys_symlink

任务是创建一个文件路径的软连接。由于这个路径不需要真的存在，故只需要将该路径直接写入软连接inode的data中去。参考sys_link()，函数代码如下：

```c++
uint64
sys_symlink(void)
{
  char name[DIRSIZ], new[MAXPATH],old[MAXPATH];
  struct inode *ip=0, *dp=0;
  int len=0;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0) {
    return -1;
  }
  begin_op();
  // 解析新路径父目录
  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  // 创建inode
  ilock(dp);
  ip = ialloc(dp->dev, T_SYMLINK);
  if (ip == 0) {
    goto bad;
  }
  ilock(ip);
  ip->nlink = 1;
  // 将old路径写入inode的data中
  len = strlen(old) + 1;
  writei(ip, 0, (uint64)old, 0, len);
  ip->size = len;
  iupdate(ip);
  // 创建目录项
  if(dirlink(dp, name, ip->inum) < 0){
    goto bad;
  }

  iunlockput(ip);
  iunlockput(dp);
  
  end_op();
  return 0;

bad:
  if (ip) {
    ip->nlink = 0;  // 标记为未使用（可选）
    iupdate(ip);
    iunlockput(ip);
  }
  if (dp)
    iunlockput(dp);
  end_op();
  return -1;
}

```

在这个函数中需要注意`inode->rev`的增删。`dp = nameiparent(new, name)) == 0`成功返回会`++dp->rev`，`ip = ialloc(dp->dev, T_SYMLINK);`成功返回会`++ip->rev`。故如果`dp` `ip`非0需要执行`iput`。

## 修改sys_open

由于软连接有递归的需要，可以另写一个函数供sys_open调用。

```c++
// kernel/sysfile.c
#define MAX_SYMLINK_DEPTH 10
static struct inode*
openat_follow(char *path, int flags, int depth) {
  if (depth > MAX_SYMLINK_DEPTH) {
    return 0; // 表示循环过深
  }

  struct inode *ip = namei(path); // 基础路径查找
  if (ip == 0) 
    return 0;
  ilock(ip);
  // 如果是符号链接且没有O_NOFOLLOW
  if (ip->type == T_SYMLINK && (flags & O_NOFOLLOW) == 0) {
    // 读取符号链接内容（target 路径）
    char target[MAXPATH];
    int n = readi(ip, 0, (uint64)target, 0, MAXPATH);
    iunlockput(ip);
    if (n <= 0)
      return 0;

    // 递归解析 target
    return openat_follow(target, flags, depth + 1);
  }
  iunlock(ip);
  return ip; // 普通文件或 O_NOFOLLOW
}
```

在`sys_open`中，将原来的`namei(path)`改为`openat_follow(path, omode, 0)`就可以运行了