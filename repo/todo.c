#include "todo.h"
#include <string.h>

#define MAX_TASKS 16

static char tasks[MAX_TASKS][32];
static int task_count = 0;

int
add_task(char *name)
{
  if(task_count >= MAX_TASKS)
    return -1;

  strcpy(tasks[task_count], name);
  task_count++;
  return 0;
}

int
delete_task(int index)
{
  int i;

  if(index < 0 || index >= task_count)
    return -1;

  for(i = index; i < task_count - 1; i++)
    strcpy(tasks[i], tasks[i + 1]);

  // BUG: missing task_count--
  return 0;
}

int
count_tasks(void)
{
  return task_count;
}
