#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;
uint all_dirs[64];
int all_dir_count;

struct path_cache_entry {
  char path[64];
  uint inum;
};

struct path_cache_entry path_cache[64];
int path_cache_count;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);
void append_dirent(uint, const char *, uint);
uint ensure_dir(const char *, uint);
void cache_path(const char *, uint);
uint lookup_path(const char *);
void pad_directory(uint);
void apply_agent_metadata(const char *, struct dinode *);

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);
  all_dirs[all_dir_count++] = rootino;
  cache_path("", rootino);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    char *shortname;
    char parentpath[64];
    char leaf[DIRSIZ + 1];
    char *slash;
    uint parentino;

    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];

    slash = strrchr(shortname, '/');
    if(slash){
      int dirlen = slash - shortname;
      assert(dirlen > 0 && dirlen < sizeof(parentpath));
      memcpy(parentpath, shortname, dirlen);
      parentpath[dirlen] = 0;
      parentino = ensure_dir(parentpath, rootino);
      strncpy(leaf, slash + 1, sizeof(leaf) - 1);
      leaf[sizeof(leaf) - 1] = 0;
    } else {
      parentpath[0] = 0;
      parentino = rootino;
      strncpy(leaf, shortname, sizeof(leaf) - 1);
      leaf[sizeof(leaf) - 1] = 0;
    }

    if((fd = open(argv[i], 0)) < 0)
      die(argv[i]);

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(leaf[0] == '_')
      memmove(leaf, leaf + 1, strlen(leaf));

    inum = ialloc(T_FILE);
    rinode(inum, &din);
    apply_agent_metadata(shortname, &din);
    winode(inum, &din);

    append_dirent(parentino, leaf, inum);

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
  }

  for(i = 0; i < all_dir_count; i++)
    pad_directory(all_dirs[i]);

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
append_dirent(uint dirino, const char *name, uint inum)
{
  struct dirent de;

  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strncpy(de.name, name, DIRSIZ);
  iappend(dirino, &de, sizeof(de));
}

uint
lookup_path(const char *path)
{
  int i;

  for(i = 0; i < path_cache_count; i++){
    if(strcmp(path_cache[i].path, path) == 0)
      return path_cache[i].inum;
  }
  return 0;
}

void
cache_path(const char *path, uint inum)
{
  assert(path_cache_count < sizeof(path_cache) / sizeof(path_cache[0]));
  strncpy(path_cache[path_cache_count].path, path,
          sizeof(path_cache[path_cache_count].path) - 1);
  path_cache[path_cache_count].path[
    sizeof(path_cache[path_cache_count].path) - 1] = 0;
  path_cache[path_cache_count].inum = inum;
  path_cache_count++;
}

uint
ensure_dir(const char *path, uint rootino)
{
  char temp[64];
  char current[64];
  char *save = 0;
  char *name;
  uint parent = rootino;

  if(path[0] == 0)
    return rootino;

  strncpy(temp, path, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = 0;
  current[0] = 0;

  for(name = strtok_r(temp, "/", &save); name; name = strtok_r(0, "/", &save)){
    uint found;
    uint inum;

    if(current[0]){
      strncat(current, "/", sizeof(current) - strlen(current) - 1);
      strncat(current, name, sizeof(current) - strlen(current) - 1);
    } else {
      strncpy(current, name, sizeof(current) - 1);
      current[sizeof(current) - 1] = 0;
    }

    found = lookup_path(current);
    if(found){
      parent = found;
      continue;
    }

    inum = ialloc(T_DIR);
    all_dirs[all_dir_count++] = inum;
    append_dirent(parent, name, inum);
    append_dirent(inum, ".", inum);
    append_dirent(inum, "..", parent);
    cache_path(current, inum);
    parent = inum;
  }

  return parent;
}

void
pad_directory(uint inum)
{
  struct dinode din;
  uint off;

  rinode(inum, &din);
  off = xint(din.size);
  off = ((off / BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(inum, &din);
}

static void
set_attr(struct dinode *din, const char *key, const char *value)
{
  int idx = din->attr_count;

  assert(idx < INODE_ATTR_MAX);
  strncpy(din->attrs[idx].key, key, sizeof(din->attrs[idx].key) - 1);
  din->attrs[idx].key[sizeof(din->attrs[idx].key) - 1] = 0;
  strncpy(din->attrs[idx].value, value, sizeof(din->attrs[idx].value) - 1);
  din->attrs[idx].value[sizeof(din->attrs[idx].value) - 1] = 0;
  din->attr_count++;
}

void
apply_agent_metadata(const char *path, struct dinode *din)
{
  memset(din->summary, 0, sizeof(din->summary));
  memset(din->attrs, 0, sizeof(din->attrs));
  din->attr_count = 0;

  if(strcmp(path, "repo/main.c") == 0){
    snprintf(din->summary, sizeof(din->summary), "%s",
             "todo app entry point using add_task, delete_task and list_tasks");
    set_attr(din, "type", "code");
    set_attr(din, "module", "todo");
    set_attr(din, "tag", "entry");
  } else if(strcmp(path, "repo/todo.c") == 0){
    snprintf(din->summary, sizeof(din->summary), "%s",
             "todo list implementation, delete_task may keep wrong task_count");
    set_attr(din, "type", "code");
    set_attr(din, "module", "todo");
    set_attr(din, "tag", "delete");
  } else if(strcmp(path, "repo/todo.h") == 0){
    snprintf(din->summary, sizeof(din->summary), "%s",
             "todo public APIs: add_task, delete_task, count_tasks");
    set_attr(din, "type", "code");
    set_attr(din, "module", "todo");
    set_attr(din, "tag", "api");
  } else if(strcmp(path, "repo/test.c") == 0){
    snprintf(din->summary, sizeof(din->summary), "%s",
             "tests for add_task, delete_task and count_tasks");
    set_attr(din, "type", "test");
    set_attr(din, "module", "todo");
    set_attr(din, "tag", "delete");
  } else if(strcmp(path, "repo/README") == 0){
    snprintf(din->summary, sizeof(din->summary), "%s",
             "todo app should update task count after delete");
    set_attr(din, "type", "doc");
    set_attr(din, "module", "todo");
    set_attr(din, "tag", "requirement");
  }
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
