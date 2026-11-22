/**
 * @file        ui/switch_libnx_thread.c
 * @brief       Horizon OS 21.0.0+ TLS ABI replacement for libnx-4.9.0's thread.o
 *
 * Horizon OS 21.0.0+ writes kernel ThreadCpuTime (e.g. negative tick deltas) at
 * tpidrro_el0 + 0x108 on context switch / preemption. libnx 4.9.0 stored its
 * 27-slot TLS array at 0x108..0x1DF, so any thread-local slot (notably slot 0
 * used by libstdc++'s __cxa_thread_atexit / blur_small_alloc) gets clobbered
 * by the kernel and faults on thread exit.
 *
 * Modern libnx (>= 4.10 / 4.12) moves the TLS slot array to tpidrro_el0 + 0x180
 * (12 slots, 0x180..0x1DF), leaving 0x100..0x17F exclusively for the kernel and
 * preserving ThreadVars at 0x1E0..0x1FF. By defining all thread.o symbols here
 * inside rexglue_switch_startup (linked ahead of -lnx), libnx.a(thread.o) is
 * never pulled into the link and the toolchain itself remains unmodified.
 */

#include <switch/arm/thread_context.h>
#include <switch/arm/tls.h>
#include <switch/kernel/mutex.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/thread.h>
#include <switch/kernel/virtmem.h>
#include <switch/kernel/wait.h>
#include <switch/result.h>
#include <switch/runtime/diag.h>
#include <switch/runtime/env.h>
#include <switch/types.h>

#include <stdatomic.h>
#include <string.h>
#include <sys/reent.h>

#define TLS_SLOTS_OFFSET  0x180u
#define NUM_TLS_SLOTS     12u
#define THREADVARS_OFFSET 0x1E0u
#define THREADVARS_MAGIC  0x21545624u /* !TV$ */
#define STACK_GUARD_SIZE  0x4000u

typedef struct {
    u32            magic;
    Handle         handle;
    Thread*        thread_ptr;
    struct _reent* reent;
    void*          tls_tp;
} ThreadVars;

typedef struct {
    Thread*        t;
    ThreadFunc     entry;
    void*          arg;
    struct _reent* reent;
    void*          tls;
    u64            pad;
} ThreadEntryArgs;

_Static_assert(sizeof(ThreadVars) == 0x20, "ThreadVars size mismatch");
_Static_assert(TLS_SLOTS_OFFSET + NUM_TLS_SLOTS * sizeof(void*) == THREADVARS_OFFSET,
               "TLS slots must end right at ThreadVars");
_Static_assert(sizeof(ThreadEntryArgs) == 0x30, "ThreadEntryArgs size mismatch");
_Static_assert(sizeof(struct _reent) == 0x180, "struct _reent size mismatch");

extern const u8     __tdata_lma[];
extern const u8     __tdata_lma_end[];
extern u8           __tls_start[];
extern u8           __tls_end[];
extern const size_t __tls_align;

extern void* __libnx_aligned_alloc(size_t alignment, size_t size);
extern void  __libnx_free(void* ptr);

static Thread            g_mainThread;
static Mutex             g_threadMutex;
static Thread*           g_threadList = NULL;
static _Atomic u64       g_tlsUsageMask = 0;
static void (*g_tlsDestructors[NUM_TLS_SLOTS])(void*);

static inline ThreadVars* getThreadVars(void) {
    return (ThreadVars*)((u8*)armGetTls() + THREADVARS_OFFSET);
}

static inline void** getTlsArray(void) {
    return (void**)((u8*)armGetTls() + TLS_SLOTS_OFFSET);
}

static size_t getTlsAlignedSize(void) {
    return ((size_t)(__tls_end - __tls_start) + 15u) & ~(size_t)15u;
}

static void _EntryWrap(ThreadEntryArgs* args) {
    Thread* t = args->t;
    void** tls_slots = getTlsArray();
    memset(tls_slots, 0, NUM_TLS_SLOTS * sizeof(void*));

    size_t align = __tls_align < 0x10u ? 0x10u : __tls_align;
    ThreadVars* tv = getThreadVars();
    tv->magic      = THREADVARS_MAGIC;
    tv->handle     = t->handle;
    tv->thread_ptr = t;
    tv->reent      = args->reent;
    tv->tls_tp     = (void*)((u8*)args->tls - align);

    mutexLock(&g_threadMutex);
    t->tls_array = tls_slots;
    t->prev_next = &g_threadList;
    t->next      = g_threadList;
    if (g_threadList != NULL) {
        g_threadList->prev_next = &t->next;
    }
    g_threadList = t;
    mutexUnlock(&g_threadMutex);

    args->entry(args->arg);
    threadExit();
}

void __libnx_init_thread(void) {
    g_mainThread.handle = envGetMainThreadHandle();

    MemoryInfo meminfo;
    u32 pageinfo = 0;
    svcQueryMemory(&meminfo, &pageinfo, (u64)(uintptr_t)&meminfo);
    g_mainThread.owns_stack_mem = false;
    g_mainThread.stack_mem      = NULL;
    g_mainThread.stack_mirror   = (void*)meminfo.addr;
    g_mainThread.stack_sz       = meminfo.size;

    void** tls_slots = getTlsArray();
    memset(tls_slots, 0, NUM_TLS_SLOTS * sizeof(void*));

    mutexLock(&g_threadMutex);
    g_mainThread.tls_array = tls_slots;
    g_mainThread.prev_next = &g_threadList;
    g_mainThread.next      = g_threadList;
    if (g_threadList != NULL) {
        g_threadList->prev_next = &g_mainThread.next;
    }
    g_threadList = &g_mainThread;
    mutexUnlock(&g_threadMutex);

    getThreadVars()->thread_ptr = &g_mainThread;
}

Result threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                    size_t stack_sz, int prio, int cpuid) {
    if ((stack_sz & 0xFFFu) != 0 || stack_sz <= sizeof(ThreadEntryArgs)) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const size_t tls_sz   = getTlsAlignedSize();
    const size_t reent_sz = sizeof(struct _reent);
    void*        backing_mem = stack_mem;
    size_t       usable_stack_sz = stack_sz;
    bool         owns_stack_mem = false;

    if (backing_mem == NULL) {
        const size_t alloc_sz =
            (stack_sz + tls_sz + reent_sz + 0xFFFu) & ~(size_t)0xFFFu;
        backing_mem = __libnx_aligned_alloc(0x1000u, alloc_sz);
        if (backing_mem == NULL) {
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        owns_stack_mem = true;
    } else {
        if (((uintptr_t)backing_mem & 0xFFFu) != 0) {
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        }
        size_t align = __tls_align < 0x10u ? 0x10u : __tls_align;
        size_t extra_sz = (reent_sz + tls_sz + align - 1u) & ~(align - 1u);
        if (extra_sz + sizeof(ThreadEntryArgs) >= stack_sz) {
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        }
        usable_stack_sz = stack_sz - extra_sz;
    }

    const size_t map_sz =
        (usable_stack_sz + tls_sz + reent_sz + 0xFFFu) & ~(size_t)0xFFFu;

    virtmemLock();
    void* stack_mirror = virtmemFindStack(map_sz, STACK_GUARD_SIZE);
    Result rc = svcMapMemory(stack_mirror, backing_mem, map_sz);
    virtmemUnlock();

    if (R_FAILED(rc)) {
        if (owns_stack_mem) {
            __libnx_free(backing_mem);
        }
        return rc;
    }

    u8*              tls   = (u8*)stack_mirror + usable_stack_sz;
    ThreadEntryArgs* args  = (ThreadEntryArgs*)(tls - sizeof(ThreadEntryArgs));
    struct _reent*   reent = (struct _reent*)(tls + tls_sz);

    t->handle         = 0;
    t->owns_stack_mem = owns_stack_mem;
    t->stack_mem      = backing_mem;
    t->stack_mirror   = stack_mirror;
    t->stack_sz       = usable_stack_sz - sizeof(ThreadEntryArgs);
    t->tls_array      = NULL;
    t->next           = NULL;
    t->prev_next      = NULL;

    args->t     = t;
    args->entry = entry;
    args->arg   = arg;
    args->reent = reent;
    args->tls   = tls;
    args->pad   = 0;

    Handle handle = 0;
    rc = svcCreateThread(&handle, (void*)_EntryWrap, args, args, prio, cpuid);
    if (R_FAILED(rc)) {
        svcUnmapMemory(stack_mirror, backing_mem, map_sz);
        if (owns_stack_mem) {
            __libnx_free(backing_mem);
        }
        return rc;
    }

    t->handle = handle;

    memset(reent, 0, reent_sz);
    _REENT_INIT_PTR(reent);
    struct _reent* cur = getThreadVars()->reent;
    if (cur != NULL) {
        reent->_stdin  = cur->_stdin;
        reent->_stdout = cur->_stdout;
        reent->_stderr = cur->_stderr;
    }

    size_t tdata_sz = (size_t)(__tdata_lma_end - __tdata_lma);
    if (tdata_sz > 0) {
        memcpy(tls, __tdata_lma, tdata_sz);
    }
    if (tls_sz > tdata_sz) {
        memset(tls + tdata_sz, 0, tls_sz - tdata_sz);
    }

    return 0;
}

Result threadStart(Thread* t) {
    return svcStartThread(t->handle);
}

void NX_NORETURN threadExit(void) {
    Thread* t = threadGetSelf();
    if (t == NULL) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_NotInitialized));
    }

    u64 mask = atomic_load_explicit(&g_tlsUsageMask, memory_order_acquire);
    for (u32 i = 0; i < NUM_TLS_SLOTS; ++i) {
        if ((mask >> i) & 1u) {
            void* val = t->tls_array[i];
            if (val != NULL) {
                t->tls_array[i] = NULL;
                if (g_tlsDestructors[i] != NULL) {
                    g_tlsDestructors[i](val);
                }
            }
        }
    }

    mutexLock(&g_threadMutex);
    if (t->prev_next != NULL) {
        *t->prev_next = t->next;
    }
    if (t->next != NULL) {
        t->next->prev_next = t->prev_next;
    }
    t->tls_array = NULL;
    t->next      = NULL;
    t->prev_next = NULL;
    mutexUnlock(&g_threadMutex);

    svcExitThread();
}

Result threadWaitForExit(Thread* t) {
    s32 idx = 0;
    return waitHandles(&idx, &t->handle, 1, UINT64_MAX);
}

Result threadClose(Thread* t) {
    if (t->tls_array != NULL) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    const size_t tls_sz = getTlsAlignedSize();
    const size_t map_sz =
        (t->stack_sz + sizeof(ThreadEntryArgs) + tls_sz + sizeof(struct _reent) + 0xFFFu) &
        ~(size_t)0xFFFu;

    Result rc = svcUnmapMemory(t->stack_mirror, t->stack_mem, map_sz);
    if (R_SUCCEEDED(rc)) {
        if (t->owns_stack_mem) {
            __libnx_free(t->stack_mem);
        }
        svcCloseHandle(t->handle);
    }
    return rc;
}

Result threadPause(Thread* t) {
    return svcSetThreadActivity(t->handle, ThreadActivity_Paused);
}

Result threadResume(Thread* t) {
    return svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
}

Result threadDumpContext(ThreadContext* ctx, Thread* t) {
    return svcGetThreadContext3(ctx, t->handle);
}

Thread* threadGetSelf(void) {
    return getThreadVars()->thread_ptr;
}

Handle threadGetCurHandle(void) {
    return getThreadVars()->handle;
}

void* threadTlsGet(s32 slot_id) {
    if ((u32)slot_id >= NUM_TLS_SLOTS) {
        return NULL;
    }
    return getTlsArray()[slot_id];
}

void threadTlsSet(s32 slot_id, void* value) {
    if ((u32)slot_id >= NUM_TLS_SLOTS) {
        return;
    }
    getTlsArray()[slot_id] = value;
}

s32 threadTlsAlloc(void (*destructor)(void*)) {
    u64 mask = atomic_load_explicit(&g_tlsUsageMask, memory_order_acquire);
    s32 slot_id;
    for (;;) {
        u64 avail = (~mask) & ((1ULL << NUM_TLS_SLOTS) - 1ULL);
        if (avail == 0) {
            return -1;
        }
        slot_id = __builtin_ctzll(avail);
        u64 new_mask = mask | (1ULL << slot_id);
        if (atomic_compare_exchange_weak_explicit(
                &g_tlsUsageMask, &mask, new_mask,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }

    threadTlsSet(slot_id, NULL);

    mutexLock(&g_threadMutex);
    for (Thread* t = g_threadList; t != NULL; t = t->next) {
        if (t->tls_array != NULL) {
            t->tls_array[slot_id] = NULL;
        }
    }
    mutexUnlock(&g_threadMutex);

    g_tlsDestructors[slot_id] = destructor;
    return slot_id;
}

void threadTlsFree(s32 slot_id) {
    if ((u32)slot_id >= NUM_TLS_SLOTS) {
        return;
    }
    g_tlsDestructors[slot_id] = NULL;
    u64 bit = 1ULL << slot_id;
    u64 mask = atomic_load_explicit(&g_tlsUsageMask, memory_order_acquire);
    while (!atomic_compare_exchange_weak_explicit(
               &g_tlsUsageMask, &mask, mask & ~bit,
               memory_order_acq_rel, memory_order_acquire)) {
    }
}
