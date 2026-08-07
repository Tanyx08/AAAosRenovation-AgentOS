#ifndef XV6_DUAL_RESULT_H
#define XV6_DUAL_RESULT_H

#define DUAL_FNV1A_OFFSET (2166136261U)
#define DUAL_FNV1A_PRIME (16777619U)

static int
dual_contains(const char *text, const char *needle)
{
  int needle_len = strlen(needle);

  if(needle_len == 0)
    return 1;
  for(; *text; text++){
    int i;

    for(i = 0; i < needle_len && text[i] == needle[i]; i++)
      ;
    if(i == needle_len)
      return 1;
  }
  return 0;
}

static int
dual_find(const char *text, const char *needle)
{
  int needle_len = strlen(needle);

  for(int pos = 0; text[pos]; pos++){
    int i;

    for(i = 0; i < needle_len && text[pos + i] == needle[i]; i++)
      ;
    if(i == needle_len)
      return pos;
  }
  return -1;
}

static int
dual_read_file(const char *path, char *buffer, int capacity)
{
  int fd;
  int total = 0;

  if(capacity <= 1)
    return -1;
  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  while(total < capacity - 1){
    int n = read(fd, buffer + total, capacity - 1 - total);

    if(n < 0){
      close(fd);
      return -1;
    }
    if(n == 0)
      break;
    total += n;
  }
  close(fd);
  buffer[total] = 0;
  return total;
}

static int
dual_file_digest(const char *path, uint32 *hash_out, int *size_out)
{
  char buffer[128];
  uint32 hash = DUAL_FNV1A_OFFSET;
  int fd;
  int total = 0;

  fd = open(path, O_RDONLY);
  if(fd < 0)
    return -1;
  for(;;){
    int n = read(fd, buffer, sizeof(buffer));

    if(n < 0){
      close(fd);
      return -1;
    }
    if(n == 0)
      break;
    for(int i = 0; i < n; i++){
      hash ^= (uchar)buffer[i];
      hash *= DUAL_FNV1A_PRIME;
    }
    total += n;
  }
  close(fd);
  *hash_out = hash;
  *size_out = total;
  return 0;
}

static void
dual_hash_hex(uint32 hash, char output[9])
{
  static const char digits[] = "0123456789abcdef";

  for(int i = 7; i >= 0; i--){
    output[i] = digits[hash & 0xf];
    hash >>= 4;
  }
  output[8] = 0;
}

static int
dual_validate_todo(const char *content)
{
  int delete_pos = dual_find(content, "delete_task(int index)");
  int next_func_pos = dual_find(content, "count_tasks(void)");
  int decrement_pos = dual_find(content, "task_count--;");
  int decrement_count = 0;
  const char *cursor = content;

  if(delete_pos < 0 || next_func_pos <= delete_pos ||
     decrement_pos <= delete_pos || decrement_pos >= next_func_pos)
    return 0;
  if(dual_contains(content, "// BUG: missing task_count--"))
    return 0;

  cursor = content;
  while(*cursor){
    int pos = dual_find(cursor, "task_count--;");

    if(pos < 0)
      break;
    decrement_count++;
    cursor += pos + strlen("task_count--;");
  }
  return decrement_count == 1;
}

#endif
