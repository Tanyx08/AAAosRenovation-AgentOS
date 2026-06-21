#include "todo.h"

int
test_delete_updates_count(void)
{
  add_task("a");
  add_task("b");
  delete_task(0);
  return count_tasks() == 1;
}
