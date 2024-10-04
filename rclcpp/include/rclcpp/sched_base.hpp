#pragma once

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <atomic>

#if defined(__x86_64__) || defined(_M_X64)
    #define PADDING_SIZE 90 
#else
    #define PADDING_SIZE 26 // Speculative, not tested
#endif

namespace rclcpp {
// forward declarations
namespace executors {
    class SingleThreadedExecutor;
}; // rclcpp::executors
namespace sched {

struct pthread_struct {
    void* __unused[PADDING_SIZE];
    pid_t tid;
};

struct SchedAttr {
    uint32_t size = sizeof(SchedAttr);              /* Size of this structure */
    uint32_t sched_policy = SCHED_OTHER;      /* Policy (SCHED_*) */
    uint64_t sched_flags;       /* Flags */
    int32_t sched_nice;        /* Nice value (SCHED_OTHER, SCHED_BATCH) */
    uint32_t sched_priority;    /* Static priority (SCHED_FIFO, SCHED_RR) */

    /* For SCHED_DEADLINE */
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;

    /* Utilization hints, unused for our purpose,
       may enable in the future*/
    // uint32_t sched_util_min;
    // uint32_t sched_util_max;
};

/** Since pthread does not expose pid to us, this is a hack to get the (linux) pid.
 *  This might be dangerous and non-portable.
 */
inline pid_t
get_pid(pthread_t threadid) {
    auto pthread_id = ((pthread_struct*) threadid);
    /* this may occur if the thread is detached from the current thread, use this
       function before calling detach() */
    if (pthread_id == nullptr) {
        printf("nullptr is passed to get_pid!\n");
        return 0;
    }
    return pthread_id->tid;
}

/** Equality for SchedAttr, use it to prevent unnecessary syscall. */
inline bool
operator==(const SchedAttr& lhs, const SchedAttr& rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(SchedAttr)) == 0;
}

/** Inequality for SchedAttr, use it to prevent unnecessary syscall. */
inline bool
operator!=(const SchedAttr& lhs, const SchedAttr& rhs) {
    return !(lhs==rhs);
}

inline long
syscall_sched_setattr(pid_t pid, const SchedAttr* sched_attr) {
    /* flags are currently unused, may enable in the future */
    return syscall(SYS_sched_setattr, pid, sched_attr, sizeof(SchedAttr), 0);
}

class SchedBase {
friend class executors::SingleThreadedExecutor;
public:
    void
    set_sched_attr(const SchedAttr& sched_attr) {
        this->sched_attr = sched_attr;
    }
protected:
    SchedAttr sched_attr;
};

}; // rclcpp::sched
}; //rclcpp
