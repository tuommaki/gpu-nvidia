/*******************************************************************************
    Copyright (c) 2016-2023 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/
#include "uvm_common.h"
#include "uvm_ioctl.h"
#include "uvm_gpu.h"
#include "uvm_hal.h"
#include "uvm_tools.h"
#include "uvm_va_space.h"
#include "uvm_api.h"
#include "uvm_hal_types.h"
#include "uvm_va_block.h"
#include "uvm_va_range.h"
#include "uvm_push.h"
#include "uvm_forward_decl.h"
#include "uvm_range_group.h"
#include "uvm_mem.h"
#include "nv_speculation_barrier.h"

// We limit the number of times a page can be retained by the kernel
// to prevent the user from maliciously passing UVM tools the same page
// over and over again in an attempt to overflow the refcount.
#define MAX_PAGE_COUNT (1 << 20)

typedef struct
{
    NvU32 get_ahead;
    NvU32 get_behind;
    NvU32 put_ahead;
    NvU32 put_behind;
} uvm_tools_queue_snapshot_t;

typedef struct
{
    uvm_spinlock_t lock;
    NvU64 subscribed_queues;
    struct list queue_nodes[UvmEventNumTypesAll];

    NvU64 *queue_buffer_pages;
    UvmEventEntry *queue;
    NvU32 queue_buffer_count;
    NvU32 notification_threshold;

    NvU64 *control_buffer_pages;
    UvmToolsEventControlData *control;

    blockq wait_queue;
    bool is_wakeup_get_valid;
    NvU32 wakeup_get;
} uvm_tools_queue_t;

typedef struct
{
    struct list counter_nodes[UVM_TOTAL_COUNTERS];
    NvU64 subscribed_counters;

    NvU64 *counter_buffer_pages;
    NvU64 *counters;

    bool all_processors;
    NvProcessorUuid processor;
} uvm_tools_counter_t;

// private_data for /dev/nvidia-uvm-tools
typedef struct
{
    bool is_queue;
    fdesc uvm_file;
    union
    {
        uvm_tools_queue_t queue;
        uvm_tools_counter_t counter;
    };
} uvm_tools_event_tracker_t;

// Delayed events
//
// Events that require gpu timestamps for asynchronous operations use a delayed
// notification mechanism. Each event type registers a callback that is invoked
// from the update_progress channel routines. The callback then enqueues a
// work item that takes care of notifying the events. This module keeps a
// global list of channels with pending events. Other modules or user apps (via
// ioctl) may call uvm_tools_flush_events to update the progress of the channels
// in the list, as needed.
//
// User apps will need to flush events before removing gpus to avoid getting
// events with gpus ids that have been removed.

// This object describes the pending migrations operations within a VA block
typedef struct
{
    nv_kthread_q_item_t queue_item;
    uvm_processor_id_t dst;
    uvm_processor_id_t src;
    uvm_va_space_t *va_space;

    uvm_channel_t *channel;
    struct list events;
    NvU64 start_timestamp_cpu;
    NvU64 end_timestamp_cpu;
    NvU64 *start_timestamp_gpu_addr;
    NvU64 start_timestamp_gpu;
    NvU64 range_group_id;
} block_migration_data_t;

// This object represents a specific pending migration within a VA block
typedef struct
{
    struct list events_node;
    NvU64 bytes;
    NvU64 address;
    NvU64 *end_timestamp_gpu_addr;
    NvU64 end_timestamp_gpu;
    UvmEventMigrationCause cause;
} migration_data_t;

// This object represents a pending gpu faut replay operation
typedef struct
{
    nv_kthread_q_item_t queue_item;
    uvm_channel_t *channel;
    uvm_gpu_id_t gpu_id;
    NvU32 batch_id;
    uvm_fault_client_type_t client_type;
    NvU64 timestamp;
    NvU64 timestamp_gpu;
    NvU64 *timestamp_gpu_addr;
} replay_data_t;

// This object describes the pending map remote operations within a VA block
typedef struct
{
    nv_kthread_q_item_t queue_item;
    uvm_processor_id_t src;
    uvm_processor_id_t dst;
    UvmEventMapRemoteCause cause;
    NvU64 timestamp;
    uvm_va_space_t *va_space;

    uvm_channel_t *channel;
    struct list events;
} block_map_remote_data_t;

// This object represents a pending map remote operation
typedef struct
{
    struct list events_node;

    NvU64 address;
    NvU64 size;
    NvU64 timestamp_gpu;
    NvU64 *timestamp_gpu_addr;
} map_remote_data_t;

typedef struct {
    file f;
    uvm_tools_event_tracker_t *tracker;
    closure_struct(fdesc_ioctl, ioctl);
    closure_struct(fdesc_close, close);
} *uvm_tools_fd;

static LIST_HEAD(g_tools_va_space_list);
static NvU32 g_tools_enabled_event_count[UvmEventNumTypesAll];
static uvm_rw_semaphore_t g_tools_va_space_list_lock;
static heap g_tools_event_tracker_cache __read_mostly = NULL;
static heap g_tools_block_migration_data_cache __read_mostly = NULL;
static heap g_tools_migration_data_cache __read_mostly = NULL;
static heap g_tools_replay_data_cache __read_mostly = NULL;
static heap g_tools_block_map_remote_data_cache __read_mostly = NULL;
static heap g_tools_map_remote_data_cache __read_mostly = NULL;
static uvm_spinlock_t g_tools_channel_list_lock;
static LIST_HEAD(g_tools_channel_list);
static nv_kthread_q_t g_tools_queue;

static NV_STATUS tools_update_status(uvm_va_space_t *va_space);

static uvm_tools_event_tracker_t *tools_event_tracker(uvm_tools_fd filp)
{
    return (uvm_tools_event_tracker_t *)atomic_long_read(&filp->tracker);
}

static bool tracker_is_queue(uvm_tools_event_tracker_t *event_tracker)
{
    return event_tracker != NULL && event_tracker->is_queue;
}

static bool tracker_is_counter(uvm_tools_event_tracker_t *event_tracker)
{
    return event_tracker != NULL && !event_tracker->is_queue;
}

static uvm_va_space_t *tools_event_tracker_va_space(uvm_tools_event_tracker_t *event_tracker)
{
    uvm_va_space_t *va_space;
    UVM_ASSERT(event_tracker->uvm_file);
    va_space = uvm_va_space_get(event_tracker->uvm_file);
    return va_space;
}

static void uvm_put_user_pages_dirty(NvU64 *pages, NvU64 page_count)
{
    NvU64 i;

    for (i = 0; i < page_count; i++) {
        set_page_dirty(pages[i]);
        NV_UNPIN_USER_PAGE(pages[i]);
    }
}

static void unmap_user_pages(NvU64 *pages, void *addr, NvU64 size)
{
    size = DIV_ROUND_UP(size, PAGE_SIZE);
    unmap(u64_from_pointer(addr), size);
    uvm_put_user_pages_dirty(pages, size);
    uvm_kvfree(pages);
}

// This must be called with the mmap_lock held in read mode or better.
static NV_STATUS check_vmas(process p, NvU64 start_va, NvU64 size)
{
    vmap vma;
    NvU64 addr = start_va;
    NvU64 region_end = start_va + size;

    do {
        vma = vmap_from_vaddr(p, addr);
        if ((vma == INVALID_ADDRESS) || !(addr >= vma->node.r.start) || uvm_file_is_nvidia_uvm(vma->fd))
            return NV_ERR_INVALID_ARGUMENT;

        addr = vma->node.r.end;
    } while (addr < region_end);

    return NV_OK;
}

// Map virtual memory of data from [user_va, user_va + size) of current process into kernel.
// Sets *addr to kernel mapping and *pages to the array of struct pages that contain the memory.
static NV_STATUS map_user_pages(NvU64 user_va, NvU64 size, void **addr, NvU64 **pages)
{
    NV_STATUS status = NV_OK;
    long ret = 0;
    long num_pages;
    long i;
    pageflags prot = pageflags_writable(pageflags_memory());

    *addr = NULL;
    *pages = NULL;
    num_pages = DIV_ROUND_UP(size, PAGE_SIZE);

    if (uvm_api_range_invalid(user_va, num_pages * PAGE_SIZE)) {
        status = NV_ERR_INVALID_ADDRESS;
        goto fail;
    }

    *pages = uvm_kvmalloc(sizeof(struct page *) * num_pages);
    if (*pages == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    status = check_vmas(current->p, user_va, size);
    if (status != NV_OK) {
        goto fail;
    }
    ret = NV_PIN_USER_PAGES(user_va, num_pages, 0, *pages, NULL);

    if (ret != num_pages) {
        status = NV_ERR_INVALID_ARGUMENT;
        goto fail;
    }

    *addr = allocate((heap)heap_virtual_page(get_kernel_heaps()), num_pages * PAGESIZE);
    if (*addr != INVALID_ADDRESS)
        for (i = 0; i < num_pages; i++)
            map(u64_from_pointer(*addr) + i * PAGESIZE, (*pages)[i], PAGESIZE, prot);
    else
        *addr = 0;
    if (*addr == NULL)
        goto fail;

    return NV_OK;

fail:
    if (*pages == NULL)
        return status;

    if (ret > 0)
        uvm_put_user_pages_dirty(*pages, ret);
    else if (ret < 0)
        status = errno_to_nv_status(ret);

    uvm_kvfree(*pages);
    *pages = NULL;
    return status;
}

static void insert_event_tracker(uvm_va_space_t *va_space,
                                 struct list *node,
                                 NvU32 list_count,
                                 NvU64 list_mask,
                                 NvU64 *subscribed_mask,
                                 struct list *lists,
                                 NvU64 *inserted_lists)
{
    NvU32 i;
    NvU64 insertable_lists = list_mask & ~*subscribed_mask;

    uvm_assert_rwsem_locked_write(&g_tools_va_space_list_lock);
    uvm_assert_rwsem_locked_write(&va_space->tools.lock);

    for (i = 0; i < list_count; i++) {
        if (insertable_lists & (1ULL << i)) {
            ++g_tools_enabled_event_count[i];
            list_add(node + i, lists + i);
        }
    }

    *subscribed_mask |= list_mask;
    *inserted_lists = insertable_lists;
}

static void remove_event_tracker(uvm_va_space_t *va_space,
                                 struct list *node,
                                 NvU32 list_count,
                                 NvU64 list_mask,
                                 NvU64 *subscribed_mask)
{
    NvU32 i;
    NvU64 removable_lists = list_mask & *subscribed_mask;

    uvm_assert_rwsem_locked_write(&g_tools_va_space_list_lock);
    uvm_assert_rwsem_locked_write(&va_space->tools.lock);

    for (i = 0; i < list_count; i++) {
        if (removable_lists & (1ULL << i)) {
            UVM_ASSERT(g_tools_enabled_event_count[i] > 0);
            --g_tools_enabled_event_count[i];
            list_del(node + i);
        }
    }

    *subscribed_mask &= ~list_mask;
}

static bool queue_needs_wakeup(uvm_tools_queue_t *queue, uvm_tools_queue_snapshot_t *sn)
{
    NvU32 queue_mask = queue->queue_buffer_count - 1;

    uvm_assert_spinlock_locked(&queue->lock);
    return ((queue->queue_buffer_count + sn->put_behind - sn->get_ahead) & queue_mask) >= queue->notification_threshold;
}

static void destroy_event_tracker(uvm_tools_event_tracker_t *event_tracker)
{
    if (event_tracker->uvm_file != NULL) {
        NV_STATUS status;
        uvm_va_space_t *va_space = tools_event_tracker_va_space(event_tracker);

        uvm_down_write(&g_tools_va_space_list_lock);
        uvm_down_write(&va_space->perf_events.lock);
        uvm_down_write(&va_space->tools.lock);

        if (event_tracker->is_queue) {
            uvm_tools_queue_t *queue = &event_tracker->queue;

            remove_event_tracker(va_space,
                                 queue->queue_nodes,
                                 UvmEventNumTypesAll,
                                 queue->subscribed_queues,
                                 &queue->subscribed_queues);

            if (queue->queue != NULL) {
                unmap_user_pages(queue->queue_buffer_pages,
                                 queue->queue,
                                 queue->queue_buffer_count * sizeof(UvmEventEntry));
            }

            if (queue->control != NULL) {
                unmap_user_pages(queue->control_buffer_pages,
                                 queue->control,
                                 sizeof(UvmToolsEventControlData));
            }
            deallocate_blockq(queue->wait_queue);
        }
        else {
            uvm_tools_counter_t *counters = &event_tracker->counter;

            remove_event_tracker(va_space,
                                 counters->counter_nodes,
                                 UVM_TOTAL_COUNTERS,
                                 counters->subscribed_counters,
                                 &counters->subscribed_counters);

            if (counters->counters != NULL) {
                unmap_user_pages(counters->counter_buffer_pages,
                                 counters->counters,
                                 UVM_TOTAL_COUNTERS * sizeof(NvU64));
            }
        }

        // de-registration should not fail
        status = tools_update_status(va_space);
        UVM_ASSERT(status == NV_OK);

        uvm_up_write(&va_space->tools.lock);
        uvm_up_write(&va_space->perf_events.lock);
        uvm_up_write(&g_tools_va_space_list_lock);

        fdesc_put(event_tracker->uvm_file);
    }
    kmem_cache_free(g_tools_event_tracker_cache, event_tracker);
}

static void enqueue_event(const UvmEventEntry *entry, uvm_tools_queue_t *queue)
{
    UvmToolsEventControlData *ctrl = queue->control;
    uvm_tools_queue_snapshot_t sn;
    NvU32 queue_size = queue->queue_buffer_count;
    NvU32 queue_mask = queue_size - 1;

    // Prevent processor speculation prior to accessing user-mapped memory to
    // avoid leaking information from side-channel attacks. There are many
    // possible paths leading to this point and it would be difficult and error-
    // prone to audit all of them to determine whether user mode could guide
    // this access to kernel memory under speculative execution, so to be on the
    // safe side we'll just always block speculation.
    nv_speculation_barrier();

    uvm_spin_lock(&queue->lock);

    // ctrl is mapped into user space with read and write permissions,
    // so its values cannot be trusted.
    sn.get_behind = atomic_read((atomic_t *)&ctrl->get_behind) & queue_mask;
    sn.put_behind = atomic_read((atomic_t *)&ctrl->put_behind) & queue_mask;
    sn.put_ahead = (sn.put_behind + 1) & queue_mask;

    // one free element means that the queue is full
    if (((queue_size + sn.get_behind - sn.put_behind) & queue_mask) == 1) {
        atomic64_inc((atomic64_t *)&ctrl->dropped + entry->eventData.eventType);
        goto unlock;
    }

    memcpy(queue->queue + sn.put_behind, entry, sizeof(*entry));

    sn.put_behind = sn.put_ahead;
    // put_ahead and put_behind will always be the same outside of queue->lock
    // this allows the user-space consumer to choose either a 2 or 4 pointer synchronization approach
    atomic_set((atomic_t *)&ctrl->put_ahead, sn.put_behind);
    atomic_set((atomic_t *)&ctrl->put_behind, sn.put_behind);

    sn.get_ahead = atomic_read((atomic_t *)&ctrl->get_ahead);
    // if the queue needs to be woken up, only signal if we haven't signaled before for this value of get_ahead
    if (queue_needs_wakeup(queue, &sn) && !(queue->is_wakeup_get_valid && queue->wakeup_get == sn.get_ahead)) {
        queue->is_wakeup_get_valid = true;
        queue->wakeup_get = sn.get_ahead;
        blockq_flush(queue->wait_queue);
    }

unlock:
    uvm_spin_unlock(&queue->lock);
}

static void uvm_tools_record_event(uvm_va_space_t *va_space, const UvmEventEntry *entry)
{
    NvU8 eventType = entry->eventData.eventType;
    uvm_tools_queue_t *queue;

    UVM_ASSERT(eventType < UvmEventNumTypesAll);

    uvm_assert_rwsem_locked(&va_space->tools.lock);

    list_for_each_entry(queue, va_space->tools.queues + eventType, queue_nodes[eventType])
        enqueue_event(entry, queue);
}

static void uvm_tools_broadcast_event(const UvmEventEntry *entry)
{
    uvm_va_space_t *va_space;

    uvm_down_read(&g_tools_va_space_list_lock);
    list_for_each_entry(va_space, &g_tools_va_space_list, tools.node) {
        uvm_down_read(&va_space->tools.lock);
        uvm_tools_record_event(va_space, entry);
        uvm_up_read(&va_space->tools.lock);
    }
    uvm_up_read(&g_tools_va_space_list_lock);
}

static bool counter_matches_processor(UvmCounterName counter, const NvProcessorUuid *processor)
{
    // For compatibility with older counters, CPU faults for memory with a preferred location are reported
    // for their preferred location as well as for the CPU device itself.
    // This check prevents double counting in the aggregate count.
    if (counter == UvmCounterNameCpuPageFaultCount)
        return uvm_processor_uuid_eq(processor, &NV_PROCESSOR_UUID_CPU_DEFAULT);
    return true;
}

static void uvm_tools_inc_counter(uvm_va_space_t *va_space,
                                  UvmCounterName counter,
                                  NvU64 amount,
                                  const NvProcessorUuid *processor)
{
    UVM_ASSERT((NvU32)counter < UVM_TOTAL_COUNTERS);
    uvm_assert_rwsem_locked(&va_space->tools.lock);

    if (amount > 0) {
        uvm_tools_counter_t *counters;

        // Prevent processor speculation prior to accessing user-mapped memory
        // to avoid leaking information from side-channel attacks. There are
        // many possible paths leading to this point and it would be difficult
        // and error-prone to audit all of them to determine whether user mode
        // could guide this access to kernel memory under speculative execution,
        // so to be on the safe side we'll just always block speculation.
        nv_speculation_barrier();

        list_for_each_entry(counters, va_space->tools.counters + counter, counter_nodes[counter]) {
            if ((counters->all_processors && counter_matches_processor(counter, processor)) ||
                uvm_processor_uuid_eq(&counters->processor, processor)) {
                atomic64_add(amount, (atomic64_t *)(counters->counters + counter));
            }
        }
    }
}

static bool tools_is_counter_enabled(uvm_va_space_t *va_space, UvmCounterName counter)
{
    uvm_assert_rwsem_locked(&va_space->tools.lock);

    UVM_ASSERT(counter < UVM_TOTAL_COUNTERS);
    return !list_empty(va_space->tools.counters + counter);
}

static bool tools_is_event_enabled(uvm_va_space_t *va_space, UvmEventType event)
{
    uvm_assert_rwsem_locked(&va_space->tools.lock);

    UVM_ASSERT(event < UvmEventNumTypesAll);
    return !list_empty(va_space->tools.queues + event);
}

static bool tools_is_event_enabled_in_any_va_space(UvmEventType event)
{
    bool ret = false;

    uvm_down_read(&g_tools_va_space_list_lock);
    ret = g_tools_enabled_event_count[event] != 0;
    uvm_up_read(&g_tools_va_space_list_lock);

    return ret;
}

static bool tools_are_enabled(uvm_va_space_t *va_space)
{
    NvU32 i;

    uvm_assert_rwsem_locked(&va_space->tools.lock);

    for (i = 0; i < UVM_TOTAL_COUNTERS; i++) {
        if (tools_is_counter_enabled(va_space, i))
            return true;
    }
    for (i = 0; i < UvmEventNumTypesAll; i++) {
        if (tools_is_event_enabled(va_space, i))
            return true;
    }
    return false;
}

static bool tools_is_fault_callback_needed(uvm_va_space_t *va_space)
{
    return tools_is_event_enabled(va_space, UvmEventTypeCpuFault) ||
           tools_is_event_enabled(va_space, UvmEventTypeGpuFault) ||
           tools_is_counter_enabled(va_space, UvmCounterNameCpuPageFaultCount) ||
           tools_is_counter_enabled(va_space, UvmCounterNameGpuPageFaultCount);
}

static bool tools_is_migration_callback_needed(uvm_va_space_t *va_space)
{
    return tools_is_event_enabled(va_space, UvmEventTypeMigration) ||
           tools_is_event_enabled(va_space, UvmEventTypeReadDuplicate) ||
           tools_is_counter_enabled(va_space, UvmCounterNameBytesXferDtH) ||
           tools_is_counter_enabled(va_space, UvmCounterNameBytesXferHtD);
}

closure_func_basic(fdesc_close, sysreturn, uvm_tools_close,
                   context ctx, io_completion completion)
{
    uvm_tools_fd filp = struct_from_field(closure_self(), uvm_tools_fd, close);
    uvm_tools_event_tracker_t *event_tracker = tools_event_tracker(filp);

    if (event_tracker != NULL) {
        destroy_event_tracker(event_tracker);
    }
    file_release(filp->f);
    NV_KFREE(filp, sizeof(*filp));
    return io_complete(completion, -nv_status_to_errno(uvm_global_get_status()));
}

closure_func_basic(fdesc_ioctl, sysreturn, uvm_tools_ioctl,
                   unsigned long cmd, vlist ap)
{
    uvm_tools_fd filp = struct_from_field(closure_self(), uvm_tools_fd, ioctl);

    uvm_thread_assert_all_unlocked();

    return ioctl_generic(&filp->f->f, cmd, ap);
}

closure_function(0, 1, sysreturn, uvm_tools_open,
                 file, f)
{
    uvm_tools_fd fd;
    NV_STATUS status = uvm_global_get_status();

    if (status != NV_OK)
        return -nv_status_to_errno(status);
    NV_KMALLOC(fd, sizeof(*fd));
    if (!fd)
        return -ENOMEM;
    f->f.ioctl = init_closure_func(&fd->ioctl, fdesc_ioctl, uvm_tools_ioctl);
    f->f.close = init_closure_func(&fd->close, fdesc_close, uvm_tools_close);
    fd->tracker = 0;
    fd->f = f;
    return 0;
}

static UvmEventFaultType g_hal_to_tools_fault_type_table[UVM_FAULT_TYPE_COUNT] = {
    [UVM_FAULT_TYPE_INVALID_PDE]          = UvmFaultTypeInvalidPde,
    [UVM_FAULT_TYPE_INVALID_PTE]          = UvmFaultTypeInvalidPte,
    [UVM_FAULT_TYPE_ATOMIC]               = UvmFaultTypeAtomic,
    [UVM_FAULT_TYPE_WRITE]                = UvmFaultTypeWrite,
    [UVM_FAULT_TYPE_PDE_SIZE]             = UvmFaultTypeInvalidPdeSize,
    [UVM_FAULT_TYPE_VA_LIMIT_VIOLATION]   = UvmFaultTypeLimitViolation,
    [UVM_FAULT_TYPE_UNBOUND_INST_BLOCK]   = UvmFaultTypeUnboundInstBlock,
    [UVM_FAULT_TYPE_PRIV_VIOLATION]       = UvmFaultTypePrivViolation,
    [UVM_FAULT_TYPE_PITCH_MASK_VIOLATION] = UvmFaultTypePitchMaskViolation,
    [UVM_FAULT_TYPE_WORK_CREATION]        = UvmFaultTypeWorkCreation,
    [UVM_FAULT_TYPE_UNSUPPORTED_APERTURE] = UvmFaultTypeUnsupportedAperture,
    [UVM_FAULT_TYPE_COMPRESSION_FAILURE]  = UvmFaultTypeCompressionFailure,
    [UVM_FAULT_TYPE_UNSUPPORTED_KIND]     = UvmFaultTypeUnsupportedKind,
    [UVM_FAULT_TYPE_REGION_VIOLATION]     = UvmFaultTypeRegionViolation,
    [UVM_FAULT_TYPE_POISONED]             = UvmFaultTypePoison,
};

// TODO: add new value for weak atomics in tools
static UvmEventMemoryAccessType g_hal_to_tools_fault_access_type_table[UVM_FAULT_ACCESS_TYPE_COUNT] = {
    [UVM_FAULT_ACCESS_TYPE_ATOMIC_STRONG] = UvmEventMemoryAccessTypeAtomic,
    [UVM_FAULT_ACCESS_TYPE_ATOMIC_WEAK]   = UvmEventMemoryAccessTypeAtomic,
    [UVM_FAULT_ACCESS_TYPE_WRITE]         = UvmEventMemoryAccessTypeWrite,
    [UVM_FAULT_ACCESS_TYPE_READ]          = UvmEventMemoryAccessTypeRead,
    [UVM_FAULT_ACCESS_TYPE_PREFETCH]      = UvmEventMemoryAccessTypePrefetch
};

static UvmEventApertureType g_hal_to_tools_aperture_table[UVM_APERTURE_MAX] = {
    [UVM_APERTURE_PEER_0] = UvmEventAperturePeer0,
    [UVM_APERTURE_PEER_1] = UvmEventAperturePeer1,
    [UVM_APERTURE_PEER_2] = UvmEventAperturePeer2,
    [UVM_APERTURE_PEER_3] = UvmEventAperturePeer3,
    [UVM_APERTURE_PEER_4] = UvmEventAperturePeer4,
    [UVM_APERTURE_PEER_5] = UvmEventAperturePeer5,
    [UVM_APERTURE_PEER_6] = UvmEventAperturePeer6,
    [UVM_APERTURE_PEER_7] = UvmEventAperturePeer7,
    [UVM_APERTURE_SYS]    = UvmEventApertureSys,
    [UVM_APERTURE_VID]    = UvmEventApertureVid,
};

static UvmEventFaultClientType g_hal_to_tools_fault_client_type_table[UVM_FAULT_CLIENT_TYPE_COUNT] = {
    [UVM_FAULT_CLIENT_TYPE_GPC] = UvmEventFaultClientTypeGpc,
    [UVM_FAULT_CLIENT_TYPE_HUB] = UvmEventFaultClientTypeHub,
};

static void record_gpu_fault_instance(uvm_gpu_t *gpu,
                                      uvm_va_space_t *va_space,
                                      const uvm_fault_buffer_entry_t *fault_entry,
                                      NvU64 batch_id,
                                      NvU64 timestamp)
{
    UvmEventEntry entry;
    UvmEventGpuFaultInfo *info = &entry.eventData.gpuFault;
    memset(&entry, 0, sizeof(entry));

    info->eventType     = UvmEventTypeGpuFault;
    info->gpuIndex      = uvm_id_value(gpu->id);
    info->faultType     = g_hal_to_tools_fault_type_table[fault_entry->fault_type];
    info->accessType    = g_hal_to_tools_fault_access_type_table[fault_entry->fault_access_type];
    info->clientType    = g_hal_to_tools_fault_client_type_table[fault_entry->fault_source.client_type];
    if (fault_entry->is_replayable)
        info->gpcId     = fault_entry->fault_source.gpc_id;
    else
        info->channelId = fault_entry->fault_source.channel_id;
    info->clientId      = fault_entry->fault_source.client_id;
    info->address       = fault_entry->fault_address;
    info->timeStamp     = timestamp;
    info->timeStampGpu  = fault_entry->timestamp;
    info->batchId       = batch_id;

    uvm_tools_record_event(va_space, &entry);
}

static void uvm_tools_record_fault(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_space_t *va_space = event_data->fault.space;

    UVM_ASSERT(event_id == UVM_PERF_EVENT_FAULT);
    UVM_ASSERT(event_data->fault.space);

    uvm_assert_rwsem_locked(&va_space->lock);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);
    UVM_ASSERT(va_space->tools.enabled);

    uvm_down_read(&va_space->tools.lock);
    UVM_ASSERT(tools_is_fault_callback_needed(va_space));

    if (UVM_ID_IS_CPU(event_data->fault.proc_id)) {
        if (tools_is_event_enabled(va_space, UvmEventTypeCpuFault)) {
            UvmEventEntry entry;
            UvmEventCpuFaultInfo *info = &entry.eventData.cpuFault;
            memset(&entry, 0, sizeof(entry));

            info->eventType = UvmEventTypeCpuFault;
            if (event_data->fault.cpu.is_write)
                info->accessType = UvmEventMemoryAccessTypeWrite;
            else
                info->accessType = UvmEventMemoryAccessTypeRead;

            info->address = event_data->fault.cpu.fault_va;
            info->timeStamp = NV_GETTIME();
            // assume that current owns va_space
            info->pid = uvm_get_stale_process_id();
            info->threadId = uvm_get_stale_thread_id();
            info->pc = event_data->fault.cpu.pc;

            uvm_tools_record_event(va_space, &entry);
        }
        if (tools_is_counter_enabled(va_space, UvmCounterNameCpuPageFaultCount)) {
            uvm_processor_id_t preferred_location;

            // The UVM Lite tools interface did not represent the CPU as a UVM
            // device. It reported CPU faults against the corresponding
            // allocation's 'home location'. Though this driver's tools
            // interface does include a CPU device, for compatibility, the
            // driver still reports faults against a buffer's preferred
            // location, in addition to the CPU.
            uvm_tools_inc_counter(va_space, UvmCounterNameCpuPageFaultCount, 1, &NV_PROCESSOR_UUID_CPU_DEFAULT);

            preferred_location = event_data->fault.preferred_location;
            if (UVM_ID_IS_GPU(preferred_location)) {
                uvm_gpu_t *gpu = uvm_va_space_get_gpu(va_space, preferred_location);
                uvm_tools_inc_counter(va_space, UvmCounterNameCpuPageFaultCount, 1, uvm_gpu_uuid(gpu));
            }
        }
    }
    else {
        uvm_gpu_t *gpu = uvm_va_space_get_gpu(va_space, event_data->fault.proc_id);
        UVM_ASSERT(gpu);

        if (tools_is_event_enabled(va_space, UvmEventTypeGpuFault)) {
            NvU64 timestamp = NV_GETTIME();
            uvm_fault_buffer_entry_t *fault_entry = event_data->fault.gpu.buffer_entry;
            uvm_fault_buffer_entry_t *fault_instance;

            record_gpu_fault_instance(gpu, va_space, fault_entry, event_data->fault.gpu.batch_id, timestamp);

            list_for_each_entry(fault_instance, &fault_entry->merged_instances_list, merged_instances_list)
                record_gpu_fault_instance(gpu, va_space, fault_instance, event_data->fault.gpu.batch_id, timestamp);
        }

        if (tools_is_counter_enabled(va_space, UvmCounterNameGpuPageFaultCount))
            uvm_tools_inc_counter(va_space, UvmCounterNameGpuPageFaultCount, 1, uvm_gpu_uuid(gpu));
    }
    uvm_up_read(&va_space->tools.lock);
}

static void add_pending_event_for_channel(uvm_channel_t *channel)
{
    uvm_assert_spinlock_locked(&g_tools_channel_list_lock);

    if (channel->tools.pending_event_count++ == 0)
        list_add_tail(&channel->tools.channel_list_node, &g_tools_channel_list);
}

static void remove_pending_event_for_channel(uvm_channel_t *channel)
{
    uvm_assert_spinlock_locked(&g_tools_channel_list_lock);
    UVM_ASSERT(channel->tools.pending_event_count > 0);
    if (--channel->tools.pending_event_count == 0)
        list_del_init(&channel->tools.channel_list_node);
}


static void record_migration_events(void *args)
{
    block_migration_data_t *block_mig = (block_migration_data_t *)args;
    migration_data_t *mig;
    migration_data_t *next;
    UvmEventEntry entry;
    UvmEventMigrationInfo *info = &entry.eventData.migration;
    uvm_va_space_t *va_space = block_mig->va_space;

    NvU64 gpu_timestamp = block_mig->start_timestamp_gpu;

    // Initialize fields that are constant throughout the whole block
    memset(&entry, 0, sizeof(entry));
    info->eventType      = UvmEventTypeMigration;
    info->srcIndex       = uvm_id_value(block_mig->src);
    info->dstIndex       = uvm_id_value(block_mig->dst);
    info->beginTimeStamp = block_mig->start_timestamp_cpu;
    info->endTimeStamp   = block_mig->end_timestamp_cpu;
    info->rangeGroupId   = block_mig->range_group_id;

    uvm_down_read(&va_space->tools.lock);
    list_for_each_entry_safe(mig, next, &block_mig->events, events_node) {
        UVM_ASSERT(mig->bytes > 0);
        list_del(&mig->events_node);

        info->address           = mig->address;
        info->migratedBytes     = mig->bytes;
        info->beginTimeStampGpu = gpu_timestamp;
        info->endTimeStampGpu   = mig->end_timestamp_gpu;
        info->migrationCause    = mig->cause;
        gpu_timestamp = mig->end_timestamp_gpu;
        kmem_cache_free(g_tools_migration_data_cache, mig);

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);

    UVM_ASSERT(list_empty(&block_mig->events));
    kmem_cache_free(g_tools_block_migration_data_cache, block_mig);
}

static void record_migration_events_entry(void *args)
{
    UVM_ENTRY_VOID(record_migration_events(args));
}

static void on_block_migration_complete(void *ptr)
{
    migration_data_t *mig;
    block_migration_data_t *block_mig = (block_migration_data_t *)ptr;

    block_mig->end_timestamp_cpu = NV_GETTIME();
    block_mig->start_timestamp_gpu = *block_mig->start_timestamp_gpu_addr;
    list_for_each_entry(mig, &block_mig->events, events_node)
        mig->end_timestamp_gpu = *mig->end_timestamp_gpu_addr;

    nv_kthread_q_item_init(&block_mig->queue_item, record_migration_events_entry, block_mig);

    // The UVM driver may notice that work in a channel is complete in a variety of situations
    // and the va_space lock is not always held in all of them, nor can it always be taken safely on them.
    // Dispatching events requires the va_space lock to be held in at least read mode, so
    // this callback simply enqueues the dispatching onto a queue, where the
    // va_space lock is always safe to acquire.
    uvm_spin_lock(&g_tools_channel_list_lock);
    remove_pending_event_for_channel(block_mig->channel);
    nv_kthread_q_schedule_q_item(&g_tools_queue, &block_mig->queue_item);
    uvm_spin_unlock(&g_tools_channel_list_lock);
}

static void record_replay_event_helper(uvm_gpu_id_t gpu_id,
                                       NvU32 batch_id,
                                       uvm_fault_client_type_t client_type,
                                       NvU64 timestamp,
                                       NvU64 timestamp_gpu)
{
    UvmEventEntry entry;

    memset(&entry, 0, sizeof(entry));
    entry.eventData.gpuFaultReplay.eventType    = UvmEventTypeGpuFaultReplay;
    entry.eventData.gpuFaultReplay.gpuIndex     = uvm_id_value(gpu_id);
    entry.eventData.gpuFaultReplay.batchId      = batch_id;
    entry.eventData.gpuFaultReplay.clientType   = g_hal_to_tools_fault_client_type_table[client_type];
    entry.eventData.gpuFaultReplay.timeStamp    = timestamp;
    entry.eventData.gpuFaultReplay.timeStampGpu = timestamp_gpu;

    uvm_tools_broadcast_event(&entry);
}

static void record_replay_events(void *args)
{
    replay_data_t *replay = (replay_data_t *)args;

    record_replay_event_helper(replay->gpu_id,
                               replay->batch_id,
                               replay->client_type,
                               replay->timestamp,
                               replay->timestamp_gpu);

    kmem_cache_free(g_tools_replay_data_cache, replay);
}

static void record_replay_events_entry(void *args)
{
    UVM_ENTRY_VOID(record_replay_events(args));
}

static void on_replay_complete(void *ptr)
{
    replay_data_t *replay = (replay_data_t *)ptr;
    replay->timestamp_gpu = *replay->timestamp_gpu_addr;

    nv_kthread_q_item_init(&replay->queue_item, record_replay_events_entry, ptr);

    uvm_spin_lock(&g_tools_channel_list_lock);
    remove_pending_event_for_channel(replay->channel);
    nv_kthread_q_schedule_q_item(&g_tools_queue, &replay->queue_item);
    uvm_spin_unlock(&g_tools_channel_list_lock);

}

static UvmEventMigrationCause g_make_resident_to_tools_migration_cause[UVM_MAKE_RESIDENT_CAUSE_MAX] = {
    [UVM_MAKE_RESIDENT_CAUSE_REPLAYABLE_FAULT]     = UvmEventMigrationCauseCoherence,
    [UVM_MAKE_RESIDENT_CAUSE_NON_REPLAYABLE_FAULT] = UvmEventMigrationCauseCoherence,
    [UVM_MAKE_RESIDENT_CAUSE_ACCESS_COUNTER]       = UvmEventMigrationCauseAccessCounters,
    [UVM_MAKE_RESIDENT_CAUSE_PREFETCH]             = UvmEventMigrationCausePrefetch,
    [UVM_MAKE_RESIDENT_CAUSE_EVICTION]             = UvmEventMigrationCauseEviction,
    [UVM_MAKE_RESIDENT_CAUSE_API_TOOLS]            = UvmEventMigrationCauseInvalid,
    [UVM_MAKE_RESIDENT_CAUSE_API_MIGRATE]          = UvmEventMigrationCauseUser,
    [UVM_MAKE_RESIDENT_CAUSE_API_SET_RANGE_GROUP]  = UvmEventMigrationCauseCoherence,
    [UVM_MAKE_RESIDENT_CAUSE_API_HINT]             = UvmEventMigrationCauseUser,
};

// This event is notified asynchronously when all the migrations pushed to the
// same uvm_push_t object in a call to block_copy_resident_pages_between have
// finished
static void uvm_tools_record_migration(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_va_block_t *va_block = event_data->migration.block;
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(va_block);

    UVM_ASSERT(event_id == UVM_PERF_EVENT_MIGRATION);

    uvm_assert_mutex_locked(&va_block->lock);
    uvm_assert_rwsem_locked(&va_space->perf_events.lock);
    UVM_ASSERT(va_space->tools.enabled);

    uvm_down_read(&va_space->tools.lock);
    UVM_ASSERT(tools_is_migration_callback_needed(va_space));

    if (tools_is_event_enabled(va_space, UvmEventTypeMigration)) {
        migration_data_t *mig;
        uvm_push_info_t *push_info = uvm_push_info_from_push(event_data->migration.push);
        block_migration_data_t *block_mig = (block_migration_data_t *)push_info->on_complete_data;

        if (push_info->on_complete != NULL) {
            mig = kmem_cache_alloc(g_tools_migration_data_cache, NV_UVM_GFP_FLAGS);
            if (mig == NULL)
                goto done_unlock;

            mig->address = event_data->migration.address;
            mig->bytes = event_data->migration.bytes;
            mig->end_timestamp_gpu_addr = uvm_push_timestamp(event_data->migration.push);
            mig->cause = g_make_resident_to_tools_migration_cause[event_data->migration.cause];

            list_add_tail(&mig->events_node, &block_mig->events);
        }
    }

    // Increment counters
    if (UVM_ID_IS_CPU(event_data->migration.src) &&
        tools_is_counter_enabled(va_space, UvmCounterNameBytesXferHtD)) {
        uvm_gpu_t *gpu = uvm_va_space_get_gpu(va_space, event_data->migration.dst);
        uvm_tools_inc_counter(va_space,
                              UvmCounterNameBytesXferHtD,
                              event_data->migration.bytes,
                              uvm_gpu_uuid(gpu));
    }
    if (UVM_ID_IS_CPU(event_data->migration.dst) &&
        tools_is_counter_enabled(va_space, UvmCounterNameBytesXferDtH)) {
        uvm_gpu_t *gpu = uvm_va_space_get_gpu(va_space, event_data->migration.src);
        uvm_tools_inc_counter(va_space,
                              UvmCounterNameBytesXferDtH,
                              event_data->migration.bytes,
                              uvm_gpu_uuid(gpu));
    }

done_unlock:
    uvm_up_read(&va_space->tools.lock);
}

// This event is notified asynchronously when it is marked as completed in the
// pushbuffer the replay method belongs to.
void uvm_tools_broadcast_replay(uvm_gpu_t *gpu,
                                uvm_push_t *push,
                                NvU32 batch_id,
                                uvm_fault_client_type_t client_type)
{
    uvm_push_info_t *push_info = uvm_push_info_from_push(push);
    replay_data_t *replay;

    // Perform delayed notification only if some VA space has signed up for
    // UvmEventTypeGpuFaultReplay
    if (!tools_is_event_enabled_in_any_va_space(UvmEventTypeGpuFaultReplay))
        return;

    replay = kmem_cache_alloc(g_tools_replay_data_cache, NV_UVM_GFP_FLAGS);
    if (replay == NULL)
        return;

    UVM_ASSERT(push_info->on_complete == NULL && push_info->on_complete_data == NULL);

    replay->timestamp_gpu_addr = uvm_push_timestamp(push);
    replay->gpu_id             = gpu->id;
    replay->batch_id           = batch_id;
    replay->client_type        = client_type;
    replay->timestamp          = NV_GETTIME();
    replay->channel            = push->channel;

    push_info->on_complete_data = replay;
    push_info->on_complete = on_replay_complete;

    uvm_spin_lock(&g_tools_channel_list_lock);
    add_pending_event_for_channel(replay->channel);
    uvm_spin_unlock(&g_tools_channel_list_lock);
}


void uvm_tools_broadcast_replay_sync(uvm_gpu_t *gpu,
                                     NvU32 batch_id,
                                     uvm_fault_client_type_t client_type)
{
    UVM_ASSERT(!gpu->parent->has_clear_faulted_channel_method);

    if (!tools_is_event_enabled_in_any_va_space(UvmEventTypeGpuFaultReplay))
        return;

    record_replay_event_helper(gpu->id,
                               batch_id,
                               client_type,
                               NV_GETTIME(),
                               gpu->parent->host_hal->get_time(gpu));
}

void uvm_tools_broadcast_access_counter(uvm_gpu_t *gpu,
                                        const uvm_access_counter_buffer_entry_t *buffer_entry,
                                        bool on_managed)
{
    UvmEventEntry entry;
    UvmEventTestAccessCounterInfo *info = &entry.testEventData.accessCounter;

    // Perform delayed notification only if some VA space has signed up for
    // UvmEventTypeAccessCounter
    if (!tools_is_event_enabled_in_any_va_space(UvmEventTypeTestAccessCounter))
        return;

    if (!buffer_entry->address.is_virtual)
        UVM_ASSERT(UVM_ID_IS_VALID(buffer_entry->physical_info.resident_id));

    memset(&entry, 0, sizeof(entry));

    info->eventType           = UvmEventTypeTestAccessCounter;
    info->srcIndex            = uvm_id_value(gpu->id);
    info->address             = buffer_entry->address.address;
    info->isVirtual           = buffer_entry->address.is_virtual? 1: 0;
    if (buffer_entry->address.is_virtual) {
        info->instancePtr         = buffer_entry->virtual_info.instance_ptr.address;
        info->instancePtrAperture = g_hal_to_tools_aperture_table[buffer_entry->virtual_info.instance_ptr.aperture];
        info->veId                = buffer_entry->virtual_info.ve_id;
    }
    else {
        info->aperture            = g_hal_to_tools_aperture_table[buffer_entry->address.aperture];
    }
    info->isFromCpu           = buffer_entry->counter_type == UVM_ACCESS_COUNTER_TYPE_MOMC? 1: 0;
    info->onManaged           = on_managed? 1 : 0;
    info->value               = buffer_entry->counter_value;
    info->subGranularity      = buffer_entry->sub_granularity;
    info->bank                = buffer_entry->bank;
    info->tag                 = buffer_entry->tag;

    uvm_tools_broadcast_event(&entry);
}

void uvm_tools_test_hmm_split_invalidate(uvm_va_space_t *va_space)
{
    UvmEventEntry entry;

    if (!va_space->tools.enabled)
        return;

    entry.testEventData.splitInvalidate.eventType = UvmEventTypeTestHmmSplitInvalidate;
    uvm_down_read(&va_space->tools.lock);
    uvm_tools_record_event(va_space, &entry);
    uvm_up_read(&va_space->tools.lock);
}

// This function is used as a begin marker to group all migrations within a VA
// block that are performed in the same call to
// block_copy_resident_pages_between. All of these are pushed to the same
// uvm_push_t object, and will be notified in burst when the last one finishes.
void uvm_tools_record_block_migration_begin(uvm_va_block_t *va_block,
                                            uvm_push_t *push,
                                            uvm_processor_id_t dst_id,
                                            uvm_processor_id_t src_id,
                                            NvU64 start,
                                            uvm_make_resident_cause_t cause)
{
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(va_block);
    uvm_range_group_range_t *range;

    // Calls from tools read/write functions to make_resident must not trigger
    // any migration
    UVM_ASSERT(cause != UVM_MAKE_RESIDENT_CAUSE_API_TOOLS);

    // During evictions the va_space lock is not held.
    if (cause != UVM_MAKE_RESIDENT_CAUSE_EVICTION)
        uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);

    // Perform delayed notification only if the VA space has signed up for
    // UvmEventTypeMigration
    if (tools_is_event_enabled(va_space, UvmEventTypeMigration)) {
        block_migration_data_t *block_mig;
        uvm_push_info_t *push_info = uvm_push_info_from_push(push);

        UVM_ASSERT(push_info->on_complete == NULL && push_info->on_complete_data == NULL);

        block_mig = kmem_cache_alloc(g_tools_block_migration_data_cache, NV_UVM_GFP_FLAGS);
        if (block_mig == NULL)
            goto done_unlock;

        block_mig->start_timestamp_gpu_addr = uvm_push_timestamp(push);
        block_mig->channel = push->channel;
        block_mig->start_timestamp_cpu = NV_GETTIME();
        block_mig->dst = dst_id;
        block_mig->src = src_id;
        block_mig->range_group_id = UVM_RANGE_GROUP_ID_NONE;

        // During evictions, it is not safe to uvm_range_group_range_find() because the va_space lock is not held.
        if (cause != UVM_MAKE_RESIDENT_CAUSE_EVICTION) {
            range = uvm_range_group_range_find(va_space, start);
            if (range != NULL)
                block_mig->range_group_id = range->range_group->id;
        }
        block_mig->va_space = va_space;

        INIT_LIST_HEAD(&block_mig->events);
        push_info->on_complete_data = block_mig;
        push_info->on_complete = on_block_migration_complete;

        uvm_spin_lock(&g_tools_channel_list_lock);
        add_pending_event_for_channel(block_mig->channel);
        uvm_spin_unlock(&g_tools_channel_list_lock);
    }

done_unlock:
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_read_duplicate(uvm_va_block_t *va_block,
                                     uvm_processor_id_t dst,
                                     uvm_va_block_region_t region,
                                     const uvm_page_mask_t *page_mask)
{
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(va_block);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeReadDuplicate)) {
        // Read-duplication events
        UvmEventEntry entry;
        UvmEventReadDuplicateInfo *info_read_duplicate = &entry.eventData.readDuplicate;
        uvm_page_index_t page_index;
        memset(&entry, 0, sizeof(entry));

        info_read_duplicate->eventType = UvmEventTypeReadDuplicate;
        info_read_duplicate->size      = PAGE_SIZE;
        info_read_duplicate->timeStamp = NV_GETTIME();

        for_each_va_block_page_in_region_mask(page_index, page_mask, region) {
            uvm_processor_id_t id;
            uvm_processor_mask_t resident_processors;

            info_read_duplicate->address    = uvm_va_block_cpu_page_address(va_block, page_index);
            info_read_duplicate->processors = 0;

            uvm_va_block_page_resident_processors(va_block, page_index, &resident_processors);
            for_each_id_in_mask(id, &resident_processors)
                info_read_duplicate->processors |= (1 << uvm_id_value(id));

            uvm_tools_record_event(va_space, &entry);
        }
    }
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_read_duplicate_invalidate(uvm_va_block_t *va_block,
                                                uvm_processor_id_t dst,
                                                uvm_va_block_region_t region,
                                                const uvm_page_mask_t *page_mask)
{
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(va_block);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeReadDuplicateInvalidate)) {
        UvmEventEntry entry;
        uvm_page_index_t page_index;
        UvmEventReadDuplicateInvalidateInfo *info = &entry.eventData.readDuplicateInvalidate;
        memset(&entry, 0, sizeof(entry));

        info->eventType     = UvmEventTypeReadDuplicateInvalidate;
        info->residentIndex = uvm_id_value(dst);
        info->size          = PAGE_SIZE;
        info->timeStamp     = NV_GETTIME();

        for_each_va_block_page_in_region_mask(page_index, page_mask, region) {
            UVM_ASSERT(uvm_page_mask_test(&va_block->read_duplicated_pages, page_index));

            info->address = uvm_va_block_cpu_page_address(va_block, page_index);
            uvm_tools_record_event(va_space, &entry);
        }
    }
    uvm_up_read(&va_space->tools.lock);
}

static void tools_schedule_completed_events(void)
{
    uvm_channel_t *channel;
    uvm_channel_t *next_channel;
    NvU64 channel_count = 0;
    NvU64 i;

    uvm_spin_lock(&g_tools_channel_list_lock);

    // retain every channel list entry currently in the list and keep track of their count.
    list_for_each_entry(channel, &g_tools_channel_list, tools.channel_list_node) {
        ++channel->tools.pending_event_count;
        ++channel_count;
    }
    uvm_spin_unlock(&g_tools_channel_list_lock);

    if (channel_count == 0)
        return;

    // new entries always appear at the end, and all the entries seen in the first loop have been retained
    // so it is safe to go through them
    channel = list_first_entry(&g_tools_channel_list, uvm_channel_t, tools.channel_list_node);
    for (i = 0; i < channel_count; i++) {
        uvm_channel_update_progress_all(channel);
        channel = list_next_entry(channel, tools.channel_list_node);
    }

    // now release all the entries we retained in the beginning
    i = 0;
    uvm_spin_lock(&g_tools_channel_list_lock);
    list_for_each_entry_safe(channel, next_channel, &g_tools_channel_list, tools.channel_list_node) {
        if (i++ == channel_count)
            break;

        remove_pending_event_for_channel(channel);
    }
    uvm_spin_unlock(&g_tools_channel_list_lock);
}

void uvm_tools_record_cpu_fatal_fault(uvm_va_space_t *va_space,
                                      NvU64 address,
                                      bool is_write,
                                      UvmEventFatalReason reason)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeFatalFault)) {
        UvmEventEntry entry;
        UvmEventFatalFaultInfo *info = &entry.eventData.fatalFault;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeFatalFault;
        info->processorIndex = UVM_ID_CPU_VALUE;
        info->timeStamp      = NV_GETTIME();
        info->address        = address;
        info->accessType     = is_write? UvmEventMemoryAccessTypeWrite: UvmEventMemoryAccessTypeRead;
        // info->faultType is not valid for cpu faults
        info->reason         = reason;

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_gpu_fatal_fault(uvm_gpu_id_t gpu_id,
                                      uvm_va_space_t *va_space,
                                      const uvm_fault_buffer_entry_t *buffer_entry,
                                      UvmEventFatalReason reason)
{
    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeFatalFault)) {
        UvmEventEntry entry;
        UvmEventFatalFaultInfo *info = &entry.eventData.fatalFault;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeFatalFault;
        info->processorIndex = uvm_id_value(gpu_id);
        info->timeStamp      = NV_GETTIME();
        info->address        = buffer_entry->fault_address;
        info->accessType     = g_hal_to_tools_fault_access_type_table[buffer_entry->fault_access_type];
        info->faultType      = g_hal_to_tools_fault_type_table[buffer_entry->fault_type];
        info->reason         = reason;

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_thrashing(uvm_va_space_t *va_space,
                                NvU64 address,
                                size_t region_size,
                                const uvm_processor_mask_t *processors)
{
    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(region_size > 0);

    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeThrashingDetected)) {
        UvmEventEntry entry;
        UvmEventThrashingDetectedInfo *info = &entry.eventData.thrashing;
        memset(&entry, 0, sizeof(entry));

        info->eventType = UvmEventTypeThrashingDetected;
        info->address   = address;
        info->size      = region_size;
        info->timeStamp = NV_GETTIME();
        bitmap_copy((long unsigned *)&info->processors, processors->bitmap, UVM_ID_MAX_PROCESSORS);

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_throttling_start(uvm_va_space_t *va_space, NvU64 address, uvm_processor_id_t processor)
{
    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(UVM_ID_IS_VALID(processor));

    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeThrottlingStart)) {
        UvmEventEntry entry;
        UvmEventThrottlingStartInfo *info = &entry.eventData.throttlingStart;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeThrottlingStart;
        info->processorIndex = uvm_id_value(processor);
        info->address        = address;
        info->timeStamp      = NV_GETTIME();

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);
}

void uvm_tools_record_throttling_end(uvm_va_space_t *va_space, NvU64 address, uvm_processor_id_t processor)
{
    UVM_ASSERT(address);
    UVM_ASSERT(PAGE_ALIGNED(address));
    UVM_ASSERT(UVM_ID_IS_VALID(processor));

    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (tools_is_event_enabled(va_space, UvmEventTypeThrottlingEnd)) {
        UvmEventEntry entry;
        UvmEventThrottlingEndInfo *info = &entry.eventData.throttlingEnd;
        memset(&entry, 0, sizeof(entry));

        info->eventType      = UvmEventTypeThrottlingEnd;
        info->processorIndex = uvm_id_value(processor);
        info->address        = address;
        info->timeStamp      = NV_GETTIME();

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);
}

static void record_map_remote_events(void *args)
{
    block_map_remote_data_t *block_map_remote = (block_map_remote_data_t *)args;
    map_remote_data_t *map_remote, *next;
    UvmEventEntry entry;
    uvm_va_space_t *va_space = block_map_remote->va_space;

    memset(&entry, 0, sizeof(entry));

    entry.eventData.mapRemote.eventType      = UvmEventTypeMapRemote;
    entry.eventData.mapRemote.srcIndex       = uvm_id_value(block_map_remote->src);
    entry.eventData.mapRemote.dstIndex       = uvm_id_value(block_map_remote->dst);
    entry.eventData.mapRemote.mapRemoteCause = block_map_remote->cause;
    entry.eventData.mapRemote.timeStamp      = block_map_remote->timestamp;

    uvm_down_read(&va_space->tools.lock);
    list_for_each_entry_safe(map_remote, next, &block_map_remote->events, events_node) {
        list_del(&map_remote->events_node);

        entry.eventData.mapRemote.address      = map_remote->address;
        entry.eventData.mapRemote.size         = map_remote->size;
        entry.eventData.mapRemote.timeStampGpu = map_remote->timestamp_gpu;
        kmem_cache_free(g_tools_map_remote_data_cache, map_remote);

        uvm_tools_record_event(va_space, &entry);
    }
    uvm_up_read(&va_space->tools.lock);

    UVM_ASSERT(list_empty(&block_map_remote->events));
    kmem_cache_free(g_tools_block_map_remote_data_cache, block_map_remote);
}

static void record_map_remote_events_entry(void *args)
{
    UVM_ENTRY_VOID(record_map_remote_events(args));
}

static void on_map_remote_complete(void *ptr)
{
    block_map_remote_data_t *block_map_remote = (block_map_remote_data_t *)ptr;
    map_remote_data_t *map_remote;

    // Only GPU mappings use the deferred mechanism
    UVM_ASSERT(UVM_ID_IS_GPU(block_map_remote->src));
    list_for_each_entry(map_remote, &block_map_remote->events, events_node)
        map_remote->timestamp_gpu = *map_remote->timestamp_gpu_addr;

    nv_kthread_q_item_init(&block_map_remote->queue_item, record_map_remote_events_entry, ptr);

    uvm_spin_lock(&g_tools_channel_list_lock);
    remove_pending_event_for_channel(block_map_remote->channel);
    nv_kthread_q_schedule_q_item(&g_tools_queue, &block_map_remote->queue_item);
    uvm_spin_unlock(&g_tools_channel_list_lock);
}

void uvm_tools_record_map_remote(uvm_va_block_t *va_block,
                                 uvm_push_t *push,
                                 uvm_processor_id_t processor,
                                 uvm_processor_id_t residency,
                                 NvU64 address,
                                 size_t region_size,
                                 UvmEventMapRemoteCause cause)
{
    uvm_va_space_t *va_space = uvm_va_block_get_va_space(va_block);

    UVM_ASSERT(UVM_ID_IS_VALID(processor));
    UVM_ASSERT(UVM_ID_IS_VALID(residency));
    UVM_ASSERT(cause != UvmEventMapRemoteCauseInvalid);

    uvm_assert_rwsem_locked(&va_space->lock);

    if (!va_space->tools.enabled)
        return;

    uvm_down_read(&va_space->tools.lock);
    if (!tools_is_event_enabled(va_space, UvmEventTypeMapRemote))
        goto done;

    if (UVM_ID_IS_CPU(processor)) {
        UvmEventEntry entry;
        memset(&entry, 0, sizeof(entry));

        entry.eventData.mapRemote.eventType      = UvmEventTypeMapRemote;
        entry.eventData.mapRemote.srcIndex       = uvm_id_value(processor);
        entry.eventData.mapRemote.dstIndex       = uvm_id_value(residency);
        entry.eventData.mapRemote.mapRemoteCause = cause;
        entry.eventData.mapRemote.timeStamp      = NV_GETTIME();
        entry.eventData.mapRemote.address        = address;
        entry.eventData.mapRemote.size           = region_size;
        entry.eventData.mapRemote.timeStampGpu   = 0;

        UVM_ASSERT(entry.eventData.mapRemote.mapRemoteCause != UvmEventMapRemoteCauseInvalid);

        uvm_tools_record_event(va_space, &entry);
    }
    else {
        uvm_push_info_t *push_info = uvm_push_info_from_push(push);
        block_map_remote_data_t *block_map_remote;
        map_remote_data_t *map_remote;

        // The first call on this pushbuffer creates the per-VA block structure
        if (push_info->on_complete == NULL) {
            UVM_ASSERT(push_info->on_complete_data == NULL);

            block_map_remote = kmem_cache_alloc(g_tools_block_map_remote_data_cache, NV_UVM_GFP_FLAGS);
            if (block_map_remote == NULL)
                goto done;

            block_map_remote->src = processor;
            block_map_remote->dst = residency;
            block_map_remote->cause = cause;
            block_map_remote->timestamp = NV_GETTIME();
            block_map_remote->va_space = va_space;
            block_map_remote->channel = push->channel;
            INIT_LIST_HEAD(&block_map_remote->events);

            push_info->on_complete_data = block_map_remote;
            push_info->on_complete = on_map_remote_complete;

            uvm_spin_lock(&g_tools_channel_list_lock);
            add_pending_event_for_channel(block_map_remote->channel);
            uvm_spin_unlock(&g_tools_channel_list_lock);
        }
        else {
            block_map_remote = push_info->on_complete_data;
        }
        UVM_ASSERT(block_map_remote);

        map_remote = kmem_cache_alloc(g_tools_map_remote_data_cache, NV_UVM_GFP_FLAGS);
        if (map_remote == NULL)
            goto done;

        map_remote->address = address;
        map_remote->size = region_size;
        map_remote->timestamp_gpu_addr = uvm_push_timestamp(push);

        list_add_tail(&map_remote->events_node, &block_map_remote->events);
    }

done:
    uvm_up_read(&va_space->tools.lock);
}

static NV_STATUS tools_update_perf_events_callbacks(uvm_va_space_t *va_space)
{
    NV_STATUS status;

    uvm_assert_rwsem_locked_write(&va_space->perf_events.lock);
    uvm_assert_rwsem_locked_write(&va_space->tools.lock);

    if (tools_is_fault_callback_needed(va_space)) {
        if (!uvm_perf_is_event_callback_registered(&va_space->perf_events, UVM_PERF_EVENT_FAULT, uvm_tools_record_fault)) {
            status = uvm_perf_register_event_callback_locked(&va_space->perf_events,
                                                             UVM_PERF_EVENT_FAULT,
                                                             uvm_tools_record_fault);

            if (status != NV_OK)
                return status;
        }
    }
    else {
        if (uvm_perf_is_event_callback_registered(&va_space->perf_events, UVM_PERF_EVENT_FAULT, uvm_tools_record_fault)) {
            uvm_perf_unregister_event_callback_locked(&va_space->perf_events,
                                                      UVM_PERF_EVENT_FAULT,
                                                      uvm_tools_record_fault);
        }
    }

    if (tools_is_migration_callback_needed(va_space)) {
        if (!uvm_perf_is_event_callback_registered(&va_space->perf_events, UVM_PERF_EVENT_MIGRATION, uvm_tools_record_migration)) {
            status = uvm_perf_register_event_callback_locked(&va_space->perf_events,
                                                             UVM_PERF_EVENT_MIGRATION,
                                                             uvm_tools_record_migration);

            if (status != NV_OK)
                return status;
        }
    }
    else {
        if (uvm_perf_is_event_callback_registered(&va_space->perf_events, UVM_PERF_EVENT_MIGRATION, uvm_tools_record_migration)) {
            uvm_perf_unregister_event_callback_locked(&va_space->perf_events,
                                                      UVM_PERF_EVENT_MIGRATION,
                                                      uvm_tools_record_migration);
        }
    }

    return NV_OK;
}

static NV_STATUS tools_update_status(uvm_va_space_t *va_space)
{
    NV_STATUS status;
    bool should_be_enabled;
    uvm_assert_rwsem_locked_write(&g_tools_va_space_list_lock);
    uvm_assert_rwsem_locked_write(&va_space->perf_events.lock);
    uvm_assert_rwsem_locked_write(&va_space->tools.lock);

    status = tools_update_perf_events_callbacks(va_space);
    if (status != NV_OK)
        return status;

    should_be_enabled = tools_are_enabled(va_space);
    if (should_be_enabled != va_space->tools.enabled) {
        if (should_be_enabled)
            list_add(&va_space->tools.node, &g_tools_va_space_list);
        else
            list_del(&va_space->tools.node);

        va_space->tools.enabled = should_be_enabled;
    }

    return NV_OK;
}

#define EVENT_FLAGS_BITS (sizeof(NvU64) * 8)

static bool mask_contains_invalid_events(NvU64 event_flags)
{
    const unsigned long *event_mask = (const unsigned long *)&event_flags;
    DECLARE_BITMAP(helper_mask, EVENT_FLAGS_BITS);
    DECLARE_BITMAP(valid_events_mask, EVENT_FLAGS_BITS);
    DECLARE_BITMAP(tests_events_mask, EVENT_FLAGS_BITS);

    bitmap_zero(tests_events_mask, EVENT_FLAGS_BITS);
    bitmap_set(tests_events_mask,
               UvmEventTestTypesFirst,
               UvmEventTestTypesLast - UvmEventTestTypesFirst + 1);

    bitmap_zero(valid_events_mask, EVENT_FLAGS_BITS);
    bitmap_set(valid_events_mask, 1, UvmEventNumTypes - 1);

    if (uvm_enable_builtin_tests)
        bitmap_or(valid_events_mask, valid_events_mask, tests_events_mask, EVENT_FLAGS_BITS);

    // Make sure that test event ids do not overlap with regular events
    BUILD_BUG_ON(UvmEventTestTypesFirst < UvmEventNumTypes);
    BUILD_BUG_ON(UvmEventTestTypesFirst > UvmEventTestTypesLast);
    BUILD_BUG_ON(UvmEventTestTypesLast >= UvmEventNumTypesAll);

    // Make sure that no test event ever changes the size of UvmEventEntry
    BUILD_BUG_ON(sizeof(((UvmEventEntry *)NULL)->testEventData) >
                 sizeof(((UvmEventEntry *)NULL)->eventData));
    BUILD_BUG_ON(UvmEventNumTypesAll > EVENT_FLAGS_BITS);

    if (!bitmap_andnot(helper_mask, event_mask, valid_events_mask, EVENT_FLAGS_BITS))
        return false;

    if (!uvm_enable_builtin_tests && bitmap_and(helper_mask, event_mask, tests_events_mask, EVENT_FLAGS_BITS))
        UVM_INFO_PRINT("Event index not found. Did you mean to insmod with uvm_enable_builtin_tests=1?\n");

    return true;
}

static NV_STATUS tools_access_va_block(uvm_va_block_t *va_block,
                                       uvm_va_block_context_t *block_context,
                                       NvU64 target_va,
                                       NvU64 size,
                                       bool is_write,
                                       uvm_mem_t *stage_mem)
{
    if (is_write) {
        return UVM_VA_BLOCK_LOCK_RETRY(va_block,
                                       NULL,
                                       uvm_va_block_write_from_cpu(va_block, block_context, target_va, stage_mem, size));
    }
    else {
        return UVM_VA_BLOCK_LOCK_RETRY(va_block,
                                       NULL,
                                       uvm_va_block_read_to_cpu(va_block, stage_mem, target_va, size));

    }
}

static NV_STATUS tools_access_process_memory(uvm_va_space_t *va_space,
                                             NvU64 target_va,
                                             NvU64 size,
                                             NvU64 user_va,
                                             NvU64 *bytes,
                                             bool is_write)
{
    NV_STATUS status;
    uvm_mem_t *stage_mem = NULL;
    void *stage_addr;
    uvm_global_processor_mask_t *retained_global_gpus = NULL;
    uvm_global_processor_mask_t *global_gpus = NULL;
    uvm_va_block_context_t *block_context = NULL;

    retained_global_gpus = uvm_kvmalloc(sizeof(*retained_global_gpus));
    if (retained_global_gpus == NULL)
        return NV_ERR_NO_MEMORY;

    uvm_global_processor_mask_zero(retained_global_gpus);

    global_gpus = uvm_kvmalloc(sizeof(*global_gpus));
    if (global_gpus == NULL) {
        status = NV_ERR_NO_MEMORY;
        goto exit;
    }

    status = uvm_mem_alloc_sysmem_and_map_cpu_kernel(PAGE_SIZE, NULL, &stage_mem);
    if (status != NV_OK)
        goto exit;

    block_context = uvm_va_block_context_alloc(NULL);
    if (!block_context) {
        status = NV_ERR_NO_MEMORY;
        goto exit;
    }

    stage_addr = uvm_mem_get_cpu_addr_kernel(stage_mem);
    *bytes = 0;

    while (*bytes < size) {
        uvm_gpu_t *gpu;
        uvm_va_block_t *block;
        void *user_va_start = (void *) (user_va + *bytes);
        NvU64 target_va_start = target_va + *bytes;
        NvU64 bytes_left = size - *bytes;
        NvU64 page_offset = target_va_start & (PAGE_SIZE - 1);
        NvU64 bytes_now = min(bytes_left, (NvU64)(PAGE_SIZE - page_offset));

        if (is_write) {
            runtime_memcpy(stage_addr, user_va_start, bytes_now);
        }

        // The RM flavor of the lock is needed to perform ECC checks.
        uvm_va_space_down_read_rm(va_space);
        status = uvm_va_block_find_create(va_space, UVM_PAGE_ALIGN_DOWN(target_va_start), &block_context->hmm.vma, &block);

        if (status != NV_OK)
            goto unlock_and_exit;

        uvm_va_space_global_gpus(va_space, global_gpus);

        for_each_global_gpu_in_mask(gpu, global_gpus) {

            // When CC is enabled, the staging memory cannot be mapped on the
            // GPU (it is protected sysmem), but it is still used to store the
            // unencrypted version of the page contents when the page is
            // resident on vidmem.
            if (uvm_conf_computing_mode_enabled(gpu)) {
                UVM_ASSERT(uvm_global_processor_mask_empty(retained_global_gpus));

                break;
            }
            if (uvm_global_processor_mask_test_and_set(retained_global_gpus, gpu->global_id))
                continue;

            // The retention of each GPU ensures that the staging memory is
            // freed before the unregistration of any of the GPUs is mapped on.
            // Each GPU is retained once.
            uvm_gpu_retain(gpu);

            // Accessing the VA block may result in copying data between the CPU
            // and a GPU. Conservatively add virtual mappings to all the GPUs
            // (even if those mappings may never be used) as tools read/write is
            // not on a performance critical path.
            status = uvm_mem_map_gpu_kernel(stage_mem, gpu);
            if (status != NV_OK)
                goto unlock_and_exit;
        }

        // Make sure a CPU resident page has an up to date struct page pointer.
        if (uvm_va_block_is_hmm(block)) {
            status = uvm_hmm_va_block_update_residency_info(block, NULL, UVM_PAGE_ALIGN_DOWN(target_va_start), true);
            if (status != NV_OK)
                goto unlock_and_exit;
        }

        status = tools_access_va_block(block, block_context, target_va_start, bytes_now, is_write, stage_mem);

        // For simplicity, check for ECC errors on all GPUs registered in the VA
        // space
        if (status == NV_OK)
            status = uvm_global_mask_check_ecc_error(global_gpus);

        uvm_va_space_up_read_rm(va_space);

        if (status != NV_OK)
            goto exit;

        if (!is_write) {
            // Prevent processor speculation prior to accessing user-mapped
            // memory to avoid leaking information from side-channel attacks.
            // Under speculation, a valid VA range which does not contain
            // target_va could be used, and the block index could run off the
            // end of the array. Information about the state of that kernel
            // memory could be inferred if speculative execution gets to the
            // point where the data is copied out.
            nv_speculation_barrier();

            runtime_memcpy(user_va_start, stage_addr, bytes_now);
        }

        *bytes += bytes_now;
    }

unlock_and_exit:
    if (status != NV_OK) {
        uvm_va_space_up_read_rm(va_space);
    }

exit:
    uvm_va_block_context_free(block_context);

    uvm_mem_free(stage_mem);

    uvm_global_mask_release(retained_global_gpus);

    uvm_kvfree(global_gpus);
    uvm_kvfree(retained_global_gpus);

    return status;
}

NV_STATUS uvm_api_tools_read_process_memory(UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS *params, fdesc filp)
{
    return tools_access_process_memory(uvm_va_space_get(filp),
                                       params->targetVa,
                                       params->size,
                                       params->buffer,
                                       &params->bytesRead,
                                       false);
}

NV_STATUS uvm_api_tools_write_process_memory(UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS *params, fdesc filp)
{
    return tools_access_process_memory(uvm_va_space_get(filp),
                                       params->targetVa,
                                       params->size,
                                       params->buffer,
                                       &params->bytesWritten,
                                       true);
}

NV_STATUS uvm_api_tools_get_processor_uuid_table(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS *params, fdesc filp)
{
    NvProcessorUuid *uuids;
    uvm_gpu_t *gpu;
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    uuids = uvm_kvmalloc_zero(sizeof(NvProcessorUuid) * UVM_ID_MAX_PROCESSORS);
    if (uuids == NULL)
        return NV_ERR_NO_MEMORY;

    uvm_processor_uuid_copy(&uuids[UVM_ID_CPU_VALUE], &NV_PROCESSOR_UUID_CPU_DEFAULT);
    params->count = 1;

    uvm_va_space_down_read(va_space);
    for_each_va_space_gpu(gpu, va_space) {
        uvm_processor_uuid_copy(&uuids[uvm_id_value(gpu->id)], uvm_gpu_uuid(gpu));
        if (uvm_id_value(gpu->id) + 1 > params->count)
            params->count = uvm_id_value(gpu->id) + 1;
    }
    uvm_va_space_up_read(va_space);

    runtime_memcpy((void *)params->tablePtr, uuids, sizeof(NvProcessorUuid) * params->count);
    uvm_kvfree(uuids);

    return NV_OK;
}

void uvm_tools_flush_events(void)
{
    tools_schedule_completed_events();

    nv_kthread_q_flush(&g_tools_queue);
}

NV_STATUS uvm_api_tools_flush_events(UVM_TOOLS_FLUSH_EVENTS_PARAMS *params, fdesc filp)
{
    uvm_tools_flush_events();
    return NV_OK;
}

static void _uvm_tools_destroy_cache_all(void)
{
    // The pointers are initialized to NULL,
    // it's safe to call destroy on all of them.
    kmem_cache_destroy_safe(&g_tools_event_tracker_cache);
    kmem_cache_destroy_safe(&g_tools_block_migration_data_cache);
    kmem_cache_destroy_safe(&g_tools_migration_data_cache);
    kmem_cache_destroy_safe(&g_tools_replay_data_cache);
    kmem_cache_destroy_safe(&g_tools_block_map_remote_data_cache);
    kmem_cache_destroy_safe(&g_tools_map_remote_data_cache);
}

int uvm_tools_init(dev_t uvm_base_dev)
{
    dev_t uvm_tools_dev = MKDEV(MAJOR(uvm_base_dev), NVIDIA_UVM_TOOLS_MINOR_NUMBER);
    int ret = -ENOMEM; // This will be updated later if allocations succeed
    spec_file_open open;

    uvm_init_rwsem(&g_tools_va_space_list_lock, UVM_LOCK_ORDER_TOOLS_VA_SPACE_LIST);

    g_tools_event_tracker_cache = NV_KMEM_CACHE_CREATE("uvm_tools_event_tracker_t",
                                                        uvm_tools_event_tracker_t);
    if (!g_tools_event_tracker_cache)
        goto err_cache_destroy;

    g_tools_block_migration_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_block_migration_data_t",
                                                              block_migration_data_t);
    if (!g_tools_block_migration_data_cache)
        goto err_cache_destroy;

    g_tools_migration_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_migration_data_t",
                                                        migration_data_t);
    if (!g_tools_migration_data_cache)
        goto err_cache_destroy;

    g_tools_replay_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_replay_data_t",
                                                     replay_data_t);
    if (!g_tools_replay_data_cache)
        goto err_cache_destroy;

    g_tools_block_map_remote_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_block_map_remote_data_t",
                                                               block_map_remote_data_t);
    if (!g_tools_block_map_remote_data_cache)
        goto err_cache_destroy;

    g_tools_map_remote_data_cache = NV_KMEM_CACHE_CREATE("uvm_tools_map_remote_data_t",
                                                         map_remote_data_t);
    if (!g_tools_map_remote_data_cache)
        goto err_cache_destroy;

    uvm_spin_lock_init(&g_tools_channel_list_lock, UVM_LOCK_ORDER_LEAF);

    ret = nv_kthread_q_init(&g_tools_queue, "UVM Tools Event Queue");
    if (ret < 0)
        goto err_cache_destroy;

    open = closure(heap_locked(get_kernel_heaps()), uvm_tools_open);
    assert(open != INVALID_ADDRESS);
    if (!create_special_file("/dev/nvidia-uvm-tools", open, 0, MAJOR(uvm_tools_dev))) {
        UVM_ERR_PRINT("cdev_add (major %u, minor %u) failed: %d\n", MAJOR(uvm_tools_dev),
                      MINOR(uvm_tools_dev), ret);
        goto err_stop_thread;
    }

    return ret;

err_stop_thread:
    nv_kthread_q_stop(&g_tools_queue);

err_cache_destroy:
    _uvm_tools_destroy_cache_all();
    return ret;
}

void uvm_tools_exit(void)
{
    unsigned i;

    nv_kthread_q_stop(&g_tools_queue);

    for (i = 0; i < UvmEventNumTypesAll; ++i)
        UVM_ASSERT(g_tools_enabled_event_count[i] == 0);

    UVM_ASSERT(list_empty(&g_tools_va_space_list));

    _uvm_tools_destroy_cache_all();
}
