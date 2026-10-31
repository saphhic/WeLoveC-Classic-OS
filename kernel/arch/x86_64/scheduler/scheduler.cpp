// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements kernel thread scheduling.

#include "scheduler.hpp"
#include "scheduler.h"
#include "arch/x86_64/inter/isr.h"
#include "arch/x86_64/inter/gdt.h"
#include "arch/x86_64/inter/idt.h"
#include "arch/x86_64/inter/pic.h"
#include "arch/x86_64/cpu/percpu.h"
#include "arch/x86_64/cpu/cpu.h"
#include "arch/x86_64/io/io.h"
#include "arch/x86_64/io/serial.h"
#include "arch/x86_64/io/ports.h"
#include "arch/x86_64/mm/spinlock.hpp"
#include "arch/x86_64/mm/paging/page_table.hpp"
#include "arch/x86_64/panic/panic.h"
#include "arch/common/MM/Heap/heap.hpp"
#include "arch/x86_64/syscall/syscall.h"
#include "../../../IDM/DisplayService/framebuffer.hpp"

namespace welovec::x86_64::scheduler {

namespace {
using welovec::x86_64::mm::IrqSpinLock;

constexpr uint32_t kPriorityLevels = 4;
constexpr uint32_t kNameLength = 32;
constexpr uint64_t kKernelStackSize = 16 * 1024;
constexpr uint64_t kTimerFrequency = 100;
constexpr uint64_t kPriorityBoostInterval = 500;
constexpr uint64_t kInitialRflags = UINT64_C(0x202);
constexpr uint32_t kSchedulerVector = 0xf0;
constexpr uint16_t kPitDivisor = static_cast<uint16_t>(1193182 / kTimerFrequency);
constexpr uint64_t kQuantumTicks[kPriorityLevels] = {2, 4, 8, 16};

enum class State : uint8_t { New, Ready, Running, Sleeping, Blocked, Zombie };

struct Task {
    TaskId id;
    TaskEntry entry;
    void *argument;
    void *stack;
    registers_t *saved_frame;
    uint64_t stack_top;
    uint64_t wake_tick;
    uint64_t quantum_remaining;
    uint64_t process_id;
    uint64_t syscall_user_rsp;
    uint64_t user_entry;
    uint64_t user_stack;
    uint64_t user_argument;
    int64_t exit_status;
    uint8_t priority;
    uint8_t base_priority;
    State state;
    bool boot_task;
    bool queued;
    bool user_mode;
    bool exit_notified;
    welovec::x86_64::mm::PhysicalAddress address_space;
    ExitCallback on_exit;
    void *exit_context;
    alignas(16) uint8_t fx_state[512];
    char name[kNameLength];
    Task *ready_previous;
    Task *ready_next;
    Task *all_previous;
    Task *all_next;
    Task *sleep_previous;
    Task *sleep_next;
    Task *reap_next;
};

struct ReadyQueue { Task *head; Task *tail; };

struct Manager {
    constexpr Manager()
        : queues{}, all_head(nullptr), sleep_head(nullptr), current(nullptr), lock{}, stats{},
          kernel_root{}, next_id(1), initialized(false), started(false) {}

    ReadyQueue queues[kPriorityLevels];
    Task *all_head;
    Task *sleep_head;
    Task *current;
    IrqSpinLock lock;
    Statistics stats;
    welovec::x86_64::mm::PhysicalAddress kernel_root;
    TaskId next_id;
    bool initialized;
    bool started;
};

constinit Manager gScheduler{};
Task gBootTask{};
alignas(16) uint8_t gInitialFxState[512];

extern "C" {
uint8_t * volatile scheduler_current_fx_state = nullptr;
}

extern "C" uint8_t kernel_stack_top[];
extern "C" void scheduler_reschedule_stub(void);
extern "C" void scheduler_task_trampoline(void);

void zero_bytes(void *address, uint64_t size) {
    auto *bytes = static_cast<uint8_t *>(address);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

void copy_name(char *destination, const char *source) {
    uint32_t i = 0;
    if (source) {
        while (i + 1 < kNameLength && source[i]) {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

void copy_bytes(void *destination, const void *source, uint64_t size) {
    auto *out = static_cast<uint8_t *>(destination);
    const auto *in = static_cast<const uint8_t *>(source);
    for (uint64_t i = 0; i < size; ++i) out[i] = in[i];
}

void initialize_fpu() {
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((UINT64_C(1) << 2) | (UINT64_C(1) << 3)); // Clear EM and TS.
    cr0 |= UINT64_C(1) << 1;    // Set MP.
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (UINT64_C(1) << 9) | (UINT64_C(1) << 10); // OSFXSR | OSXMMEXCPT.
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("fninit");
    const uint32_t default_mxcsr = UINT32_C(0x1f80);
    __asm__ volatile("ldmxcsr %0" :: "m"(default_mxcsr));
    __asm__ volatile("fxsave64 (%0)" :: "r"(gInitialFxState) : "memory");
}

uint64_t stack_top_for(const Task *task) {
    return task->boot_task ? reinterpret_cast<uint64_t>(kernel_stack_top) : task->stack_top;
}

void update_kernel_stack(Task *task) {
    const uint64_t top = stack_top_for(task);
    tss_set_kernel_stack(top);
    g_percpu0.kernel_rsp = top;
    g_percpu0.user_rsp = task->syscall_user_rsp;
    scheduler_current_fx_state = task->fx_state;
}

void ready_push(Task *task) {
    ReadyQueue &queue = gScheduler.queues[task->priority];
    task->state = State::Ready;
    task->queued = true;
    task->ready_previous = queue.tail;
    task->ready_next = nullptr;
    if (queue.tail) queue.tail->ready_next = task;
    else queue.head = task;
    queue.tail = task;
    ++gScheduler.stats.ready_count;
}

void ready_remove(Task *task) {
    if (!task->queued) return;
    ReadyQueue &queue = gScheduler.queues[task->priority];
    if (task->ready_previous) task->ready_previous->ready_next = task->ready_next;
    else queue.head = task->ready_next;
    if (task->ready_next) task->ready_next->ready_previous = task->ready_previous;
    else queue.tail = task->ready_previous;
    task->ready_previous = nullptr;
    task->ready_next = nullptr;
    task->queued = false;
    --gScheduler.stats.ready_count;
}

Task *ready_pop_highest() {
    for (uint32_t priority = 0; priority < kPriorityLevels; ++priority) {
        ReadyQueue &queue = gScheduler.queues[priority];
        if (!queue.head) continue;
        Task *task = queue.head;
        ready_remove(task);
        return task;
    }
    return nullptr;
}

uint32_t highest_ready_priority() {
    for (uint32_t priority = 0; priority < kPriorityLevels; ++priority)
        if (gScheduler.queues[priority].head) return priority;
    return kPriorityLevels;
}

void all_insert(Task *task) {
    task->all_previous = nullptr;
    task->all_next = gScheduler.all_head;
    if (gScheduler.all_head) gScheduler.all_head->all_previous = task;
    gScheduler.all_head = task;
}

void all_remove(Task *task) {
    if (task->all_previous) task->all_previous->all_next = task->all_next;
    else gScheduler.all_head = task->all_next;
    if (task->all_next) task->all_next->all_previous = task->all_previous;
    task->all_previous = nullptr;
    task->all_next = nullptr;
}

void sleep_remove(Task *task) {
    if (task->sleep_previous) task->sleep_previous->sleep_next = task->sleep_next;
    else gScheduler.sleep_head = task->sleep_next;
    if (task->sleep_next) task->sleep_next->sleep_previous = task->sleep_previous;
    task->sleep_previous = nullptr;
    task->sleep_next = nullptr;
}

bool deadline_before(uint64_t left, uint64_t right) {
    return static_cast<int64_t>(left - right) < 0;
}

bool deadline_reached(uint64_t now, uint64_t deadline) {
    return static_cast<int64_t>(now - deadline) >= 0;
}

void sleep_insert(Task *task) {
    Task *position = gScheduler.sleep_head;
    Task *previous = nullptr;
    while (position && !deadline_before(task->wake_tick, position->wake_tick)) {
        previous = position;
        position = position->sleep_next;
    }
    task->sleep_previous = previous;
    task->sleep_next = position;
    if (previous) previous->sleep_next = task;
    else gScheduler.sleep_head = task;
    if (position) position->sleep_previous = task;
}

void wake_sleepers() {
    while (gScheduler.sleep_head && deadline_reached(gScheduler.stats.ticks, gScheduler.sleep_head->wake_tick)) {
        Task *task = gScheduler.sleep_head;
        sleep_remove(task);
        --gScheduler.stats.sleeping_count;
        if (task->priority > task->base_priority) --task->priority;
        task->quantum_remaining = 0;
        ready_push(task);
    }
}

void boost_priorities() {
    for (Task *task = gScheduler.all_head; task; task = task->all_next) {
        if (task->priority == task->base_priority) continue;
        if (task->state == State::Ready) ready_remove(task);
        task->priority = task->base_priority;
        if (task->state == State::Ready) {
            task->quantum_remaining = 0;
            ready_push(task);
        } else if (task->state == State::Running) {
            task->quantum_remaining = kQuantumTicks[task->priority];
        } else {
            task->quantum_remaining = 0;
        }
    }
}

void reap_zombies_locked(Task *active_stack_task, Task **reap_head) {
    Task *task = gScheduler.all_head;
    while (task) {
        Task *next = task->all_next;
        if (task != active_stack_task && task->state == State::Zombie) {
            all_remove(task);
            task->reap_next = *reap_head;
            *reap_head = task;
        }
        task = next;
    }
}

void notify_exit_locked(Task *task) {
    if (task->exit_notified) return;
    task->exit_notified = true;
    if (task->on_exit)
        task->on_exit(task->id, task->process_id, task->exit_status, task->exit_context);
}

void mark_zombie_locked(Task *task, int64_t status) {
    if (!task || task->state == State::Zombie) return;
    if (task->state == State::Ready) ready_remove(task);
    else if (task->state == State::Sleeping) {
        sleep_remove(task);
        --gScheduler.stats.sleeping_count;
    } else if (task->state == State::Blocked) {
        --gScheduler.stats.blocked_count;
    }
    task->state = State::Zombie;
    task->quantum_remaining = 0;
    task->exit_status = status;
    --gScheduler.stats.task_count;
    notify_exit_locked(task);
}

void reap_tasks(Task *task) {
    while (task) {
        Task *next = task->reap_next;
        void *stack = task->stack;
        welovec::memory::heap::deallocate(stack);
        welovec::memory::heap::deallocate(task);
        task = next;
    }
}

void initialize_frame(Task *task) {
    const uint64_t frame_address = (task->stack_top - sizeof(registers_t)) & ~UINT64_C(0xf);
    auto *frame = reinterpret_cast<registers_t *>(frame_address);
    zero_bytes(frame, sizeof(*frame));
    copy_bytes(task->fx_state, gInitialFxState, sizeof(task->fx_state));
    frame->int_no = kSchedulerVector;
    frame->rflags = kInitialRflags;
    // The first iret frame is synthetic. Give the kernel-thread frame a real
    // stack pointer too; otherwise the first call in the trampoline can push
    // its return address through RSP=0 and escalate a page fault to #DF.
    frame->user_rsp = task->stack_top;
    if (task->user_mode) {
        frame->rdi = task->user_argument;
        frame->cs = GDT_SEL_UCODE | 3;
        frame->ss = GDT_SEL_UDATA | 3;
        frame->user_rsp = task->user_stack;
        frame->rip = task->user_entry;
    } else {
        frame->rdi = reinterpret_cast<uint64_t>(task);
        frame->cs = GDT_SEL_KCODE;
        frame->ss = GDT_SEL_KDATA;
        frame->rip = reinterpret_cast<uint64_t>(scheduler_task_trampoline);
    }
    task->saved_frame = frame;
}

TaskId create_task(const char *name, TaskEntry entry, void *argument, uint64_t user_entry,
                   uint64_t user_stack, uint64_t user_argument, uint64_t process_id,
                   welovec::x86_64::mm::PhysicalAddress address_space, ExitCallback on_exit,
                   void *exit_context, Priority priority, bool user_mode) {
    const uint8_t priority_value = static_cast<uint8_t>(priority);
    if (!gScheduler.initialized || priority_value >= kPriorityLevels ||
        (user_mode && (!process_id || !address_space.valid() || !user_entry || !user_stack)) ||
        (!user_mode && !entry)) return 0;

    auto *task = static_cast<Task *>(welovec::memory::heap::allocate(sizeof(Task)));
    if (!task) return 0;
    zero_bytes(task, sizeof(*task));
    task->stack = welovec::memory::heap::allocate(kKernelStackSize);
    if (!task->stack) {
        welovec::memory::heap::deallocate(task);
        return 0;
    }
    task->stack_top = (reinterpret_cast<uint64_t>(task->stack) + kKernelStackSize) & ~UINT64_C(0xf);
    task->entry = entry;
    task->argument = argument;
    task->user_entry = user_entry;
    task->user_stack = user_stack;
    task->user_argument = user_argument;
    task->process_id = process_id;
    task->address_space = address_space;
    task->user_mode = user_mode;
    task->on_exit = on_exit;
    task->exit_context = exit_context;
    task->priority = priority_value;
    task->base_priority = priority_value;
    task->state = State::New;
    copy_name(task->name, name);
    initialize_frame(task);

    const uint64_t irq = gScheduler.lock.lock_irqsave();
    if (gScheduler.next_id == 0 || gScheduler.next_id == UINT64_MAX) {
        gScheduler.lock.unlock_irqrestore(irq);
        welovec::memory::heap::deallocate(task->stack);
        welovec::memory::heap::deallocate(task);
        return 0;
    }
    task->id = gScheduler.next_id++;
    all_insert(task);
    ++gScheduler.stats.task_count;
    ready_push(task);
    gScheduler.lock.unlock_irqrestore(irq);
    return task->id;
}

void configure_pit() {
    outb(PIT_COMMAND, 0x36); // Channel 0, lobyte/hibyte, square-wave mode.
    outb(PIT_CHANNEL0, static_cast<uint8_t>(kPitDivisor & 0xff));
    outb(PIT_CHANNEL0, static_cast<uint8_t>(kPitDivisor >> 8));
}

void configure_pic() {
    pic_remap(0x20, 0x28);
    for (uint8_t irq = 0; irq < 16; ++irq) pic_set_mask(irq);
    pic_clear_mask(0);
}

registers_t *dispatch(registers_t *frame, bool timer_tick) {
    if (!frame) return frame;
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    if (!gScheduler.started || !gScheduler.current) {
        gScheduler.lock.unlock_irqrestore(irq);
        return frame;
    }

    Task *outgoing = gScheduler.current;
    outgoing->saved_frame = frame;
    outgoing->syscall_user_rsp = g_percpu0.user_rsp;
    if (timer_tick) {
        ++gScheduler.stats.ticks;
        wake_sleepers();
        if (gScheduler.stats.ticks % kPriorityBoostInterval == 0) boost_priorities();
    }

    if (timer_tick && outgoing->state == State::Running && outgoing->boot_task &&
        highest_ready_priority() == kPriorityLevels) {
        Task *reap_head = nullptr;
        reap_zombies_locked(outgoing, &reap_head);
        gScheduler.lock.unlock_irqrestore(irq);
        reap_tasks(reap_head);
        return frame;
    }

    if (timer_tick && outgoing->state == State::Running && !outgoing->boot_task) {
        const uint32_t ready_priority = highest_ready_priority();
        if (ready_priority >= outgoing->priority && outgoing->quantum_remaining > 1) {
            --outgoing->quantum_remaining;
            gScheduler.lock.unlock_irqrestore(irq);
            return frame;
        }
        if (ready_priority < outgoing->priority) {
            ready_push(outgoing); // A newly ready higher-priority task preempts immediately.
        } else {
            outgoing->quantum_remaining = 0;
            if (outgoing->priority + 1 < kPriorityLevels) ++outgoing->priority;
            ready_push(outgoing); // Consuming a full slice demotes the task one MLFQ level.
        }
    } else if (outgoing->state == State::Running) {
        if (outgoing->boot_task) outgoing->state = State::Ready;
        else ready_push(outgoing);
    }

    Task *next = ready_pop_highest();
    if (!next) next = &gBootTask;
    if (next->state == State::Zombie || !next->saved_frame) {
        gScheduler.lock.unlock_irqrestore(irq);
        return frame;
    }

    next->state = State::Running;
    if (!next->boot_task && !next->quantum_remaining)
        next->quantum_remaining = kQuantumTicks[next->priority];
    gScheduler.current = next;
    const auto target_root = next->user_mode ? next->address_space : gScheduler.kernel_root;
    if (welovec::x86_64::mm::paging::active_root() != target_root &&
        !welovec::x86_64::mm::paging::activate_address_space(target_root)) {
        gScheduler.lock.unlock_irqrestore(irq);
        panic("scheduler could not activate task address space", frame);
    }
    update_kernel_stack(next);
    if (next != outgoing) {
        ++gScheduler.stats.context_switches;
        if (timer_tick) ++gScheduler.stats.preemptions;
    }

    Task *reap_head = nullptr;
    reap_zombies_locked(outgoing, &reap_head);
    registers_t *next_frame = next->saved_frame;
    gScheduler.lock.unlock_irqrestore(irq);
    reap_tasks(reap_head);
    return next_frame;
}

void software_reschedule() {
    __asm__ volatile("int $0xf0" ::: "memory");
}

}

bool initialize() {
    if (!welovec::memory::heap::ready()) return false;
    if (gScheduler.initialized) return true;

    cli();
    serial_init();
    initialize_fpu();
    gdt_init();
    idt_init();
    percpu_init(reinterpret_cast<uint64_t>(kernel_stack_top));
    tss_set_kernel_stack(reinterpret_cast<uint64_t>(kernel_stack_top));
    idt_set_gate(kSchedulerVector, reinterpret_cast<uint64_t>(scheduler_reschedule_stub),
                 GDT_SEL_KCODE, 0, IDT_GATE_INTERRUPT);
    syscall_init();
    configure_pic();
    configure_pit();

    const uint64_t irq = gScheduler.lock.lock_irqsave();
    zero_bytes(&gBootTask, sizeof(gBootTask));
    gBootTask.address_space = welovec::x86_64::mm::PhysicalAddress();
    copy_bytes(gBootTask.fx_state, gInitialFxState, sizeof(gBootTask.fx_state));
    gBootTask.id = 0;
    gBootTask.name[0] = 'i';
    gBootTask.name[1] = 'd';
    gBootTask.name[2] = 'l';
    gBootTask.name[3] = 'e';
    gBootTask.state = State::Ready;
    gBootTask.priority = static_cast<uint8_t>(Priority::Low);
    gBootTask.base_priority = gBootTask.priority;
    gBootTask.boot_task = true;
    gScheduler.kernel_root = welovec::x86_64::mm::paging::active_root();
    gScheduler.initialized = true;
    gScheduler.lock.unlock_irqrestore(irq);
    return true;
}

void start() {
    if (!gScheduler.initialized) return;
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    if (gScheduler.started) {
        gScheduler.lock.unlock_irqrestore(irq);
        return;
    }
    gScheduler.started = true;
    gScheduler.current = &gBootTask;
    gBootTask.state = State::Running;
    update_kernel_stack(&gBootTask);
    gScheduler.lock.unlock_irqrestore(irq);

    // STI's one-instruction shadow ensures the software reschedule captures
    // this stack before a pending PIT IRQ can arrive.
    __asm__ volatile("sti; int $0xf0" ::: "memory");
}

bool ready() { return gScheduler.initialized; }
bool running() { return gScheduler.started; }

TaskId create_kernel_thread(const char *name, TaskEntry entry, void *argument, Priority priority) {
    return create_task(name, entry, argument, 0, 0, 0, 0,
                       welovec::x86_64::mm::PhysicalAddress(), nullptr, nullptr,
                       priority, false);
}

TaskId create_user_thread(const char *name, uint64_t entry, uint64_t user_stack,
                          uint64_t argument, uint64_t process_id,
                          welovec::x86_64::mm::PhysicalAddress address_space,
                          ExitCallback on_exit, void *exit_context, Priority priority) {
    const uint64_t user_limit = welovec::x86_64::mm::paging::active_levels() == 5
        ? UINT64_C(0x0100000000000000) : UINT64_C(0x0000800000000000);
    if (!entry || entry >= user_limit || !user_stack || user_stack >= user_limit ||
        (user_stack & 0x7) != 0 || !process_id || !address_space.valid()) return 0;
    return create_task(name, nullptr, nullptr, entry, user_stack, argument, process_id,
                       address_space, on_exit, exit_context, priority, true);
}

bool terminate_group(uint64_t process_id, int64_t status) {
    if (!process_id || !gScheduler.initialized) return false;
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    bool found = false;
    for (Task *task = gScheduler.all_head; task; task = task->all_next) {
        if (task->process_id != process_id || task->state == State::Zombie) continue;
        found = true;
        mark_zombie_locked(task, status);
    }
    const bool terminate_current = gScheduler.current &&
        gScheduler.current->process_id == process_id && gScheduler.current->state == State::Zombie;
    gScheduler.lock.unlock_irqrestore(irq);
    if (terminate_current) {
        software_reschedule();
        cli();
        for (;;) hlt();
    }
    return found;
}

void yield() {
    if (gScheduler.started) software_reschedule();
}

bool sleep(uint64_t duration) {
    if (!gScheduler.started || !gScheduler.current || gScheduler.current->boot_task) return false;
    if (!duration) { yield(); return true; }
    if (duration > static_cast<uint64_t>(INT64_MAX)) return false;

    const uint64_t irq = gScheduler.lock.lock_irqsave();
    Task *task = gScheduler.current;
    task->state = State::Sleeping;
    task->wake_tick = gScheduler.stats.ticks + duration;
    task->quantum_remaining = 0;
    if (task->priority > task->base_priority) --task->priority;
    sleep_insert(task);
    ++gScheduler.stats.sleeping_count;
    gScheduler.lock.unlock_irqrestore(irq);
    software_reschedule();
    return true;
}

bool block_current() {
    if (!gScheduler.started || !gScheduler.current || gScheduler.current->boot_task) return false;
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    gScheduler.current->state = State::Blocked;
    gScheduler.current->quantum_remaining = 0;
    if (gScheduler.current->priority > gScheduler.current->base_priority)
        --gScheduler.current->priority;
    ++gScheduler.stats.blocked_count;
    gScheduler.lock.unlock_irqrestore(irq);
    software_reschedule();
    return true;
}

bool wake(TaskId id) {
    if (!gScheduler.initialized || !id) return false;
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    Task *task = gScheduler.all_head;
    while (task && task->id != id) task = task->all_next;
    if (!task || (task->state != State::Blocked && task->state != State::Sleeping)) {
        gScheduler.lock.unlock_irqrestore(irq);
        return false;
    }
    if (task->state == State::Sleeping) {
        sleep_remove(task);
        --gScheduler.stats.sleeping_count;
    } else {
        --gScheduler.stats.blocked_count;
    }
    if (task->priority > task->base_priority) --task->priority;
    task->quantum_remaining = 0;
    ready_push(task);
    gScheduler.lock.unlock_irqrestore(irq);
    return true;
}

[[noreturn]] void exit_current() { exit_current(0); }

[[noreturn]] void exit_current(int64_t status) {
    if (!gScheduler.started || !gScheduler.current || gScheduler.current->boot_task) {
        cli();
        for (;;) hlt();
    }
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    mark_zombie_locked(gScheduler.current, status);
    gScheduler.lock.unlock_irqrestore(irq);
    software_reschedule();
    cli();
    for (;;) hlt();
}

TaskId current_id() {
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    const TaskId id = gScheduler.current ? gScheduler.current->id : 0;
    gScheduler.lock.unlock_irqrestore(irq);
    return id;
}

uint64_t current_process_id() {
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    const uint64_t id = gScheduler.current ? gScheduler.current->process_id : 0;
    gScheduler.lock.unlock_irqrestore(irq);
    return id;
}

welovec::x86_64::mm::PhysicalAddress current_address_space() {
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    const auto root = gScheduler.current && gScheduler.current->user_mode
        ? gScheduler.current->address_space : welovec::x86_64::mm::PhysicalAddress();
    gScheduler.lock.unlock_irqrestore(irq);
    return root;
}

uint64_t tick_count() {
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    const uint64_t result = gScheduler.stats.ticks;
    gScheduler.lock.unlock_irqrestore(irq);
    return result;
}

Statistics statistics() {
    const uint64_t irq = gScheduler.lock.lock_irqsave();
    const Statistics result = gScheduler.stats;
    gScheduler.lock.unlock_irqrestore(irq);
    return result;
}

extern "C" [[noreturn]] void scheduler_task_bootstrap(void *opaque_task) {
    auto *task = static_cast<Task *>(opaque_task);
    task->entry(task->argument);
    exit_current();
}

extern "C" registers_t *scheduler_timer_dispatch(registers_t *frame) {
    return dispatch(frame, true);
}

extern "C" registers_t *scheduler_reschedule_dispatch(registers_t *frame) {
    return dispatch(frame, false);
}

}

extern "C" int scheduler_initialize(void) {
    return welovec::x86_64::scheduler::initialize() ? 1 : 0;
}

extern "C" void scheduler_start(void) { welovec::x86_64::scheduler::start(); printf_fb_color(" [Done.]\n\n", FB_GREEN); }

extern "C" scheduler_task_id_t scheduler_create_kernel_thread(const char *name,
                                                                 scheduler_task_entry_t entry,
                                                                 void *argument,
                                                                 uint8_t priority) {
    if (priority > SCHEDULER_PRIORITY_LOW) return 0;
    return welovec::x86_64::scheduler::create_kernel_thread(
        name, entry, argument, static_cast<welovec::x86_64::scheduler::Priority>(priority));
}

extern "C" void scheduler_yield(void) { welovec::x86_64::scheduler::yield(); }
extern "C" int scheduler_sleep(uint64_t ticks) { return welovec::x86_64::scheduler::sleep(ticks) ? 1 : 0; }
extern "C" int scheduler_block_current(void) { return welovec::x86_64::scheduler::block_current() ? 1 : 0; }
extern "C" int scheduler_wake(scheduler_task_id_t id) { return welovec::x86_64::scheduler::wake(id) ? 1 : 0; }
extern "C" void scheduler_exit_current(void) { welovec::x86_64::scheduler::exit_current(); }
extern "C" scheduler_task_id_t scheduler_current_id(void) { return welovec::x86_64::scheduler::current_id(); }
extern "C" uint64_t scheduler_tick_count(void) { return welovec::x86_64::scheduler::tick_count(); }
