#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/slab.h>

// Meta Information
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ME");
MODULE_DESCRIPTION("A module that knows how to greet");

/*
 * module_param(foo, int, 0000)
 * The first param is the parameters name
 * The second param is its data type
 * The final argument is the permissions bits,
 * for exposing parameters in sysfs (if non-zero) at a later stage.
 */
/*
char *name;
int age;
module_param(name, charp, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(name, "name of the caller");

module_param(age, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(age, "age of the caller");

*/
int pid;
module_param(pid, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(pid, "Id of the process");

void pstree(struct task_struct *task, int pid, int eldest) {
	struct task_struct *ts;
	struct list_head *task_list;
	bool is_eldest = true;
	if (pid != 0) {
		//printk("%s,%d,%lld,%d,%lld,%d\n", task->comm, task->parent->pid,task->parent->start_time,task->pid,task->start_time,eldest);
		printk(
			"\"PID:%d, Creation Time:%lld\" ->\"PID:%d, Creation Time:%lld\"\n",
			task->parent->pid, task->parent->start_time, task->pid,
			task->start_time);
		if (eldest) {
			printk("\"PID:%d, Creation Time:%lld\"[color=blue]\n", task->pid,
				   task->start_time);
		}
	}
	list_for_each(task_list, &task->children) {
		ts = list_entry(task_list, struct task_struct, sibling);
		if (is_eldest) {
			pstree(ts, task->pid, 1);
			is_eldest = false;
		} else {
			pstree(ts, task->pid, 0);
		}
	}
}

// A function that runs when the module is first loaded
int simple_init(void) {
	struct task_struct *ts;
	ts = get_pid_task(find_get_pid(pid), PIDTYPE_PID);
	pstree(ts, 0, 0);
	return 0;
}

// A function that runs when the module is removed
void simple_exit(void) {
	printk("Goodbye from the kernel.\n");
}

module_init(simple_init);
module_exit(simple_exit);
