
#include <stddef.h>

#define MAX_TASKS 16

typedef void (*task_function_t)(void);

static task_function_t tasks[MAX_TASKS];
static size_t task_count = 0;
static size_t current_task = 0;

extern void vga_write(const char *text);

void scheduler_init(void)
{
    task_count = 0;
    current_task = 0;

    vga_write("[SCHED] Round-robin scheduler ready.\n");
}

int scheduler_add_task(task_function_t function)
{
    if (function == NULL || task_count >= MAX_TASKS) {
        return -1;
    }

    tasks[task_count++] = function;
    return 0;
}

__attribute__((noreturn))
void scheduler_run(void)
{
    if (task_count == 0) {
        vga_write("[SCHED] No tasks registered.\n");

        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }

    for (;;) {
        tasks[current_task]();

        current_task++;

        if (current_task >= task_count) {
            current_task = 0;
        }
    }
}
