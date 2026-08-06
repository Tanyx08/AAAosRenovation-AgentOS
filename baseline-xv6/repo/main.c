#include "todo.h"

int
main(void)
{
  add_task("write report");
  add_task("fix delete bug");

  list_tasks();
  delete_task(0);
  list_tasks();

  return count_tasks();
}
