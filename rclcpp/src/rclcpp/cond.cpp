#include <sys/syscall.h>
#include <unistd.h>
#include <linux/futex.h>
#include <atomic>

#include "rclcpp/cond.hpp"

inline int syncutil::Condition::futex_wait(const uint32_t val) {
    return syscall(SYS_futex, &(this->futex), FUTEX_WAIT, val, NULL);
}

inline int syncutil::Condition::futex_wake() {
    return syscall(SYS_futex, &(this->futex), FUTEX_WAKE, 1, NULL);
}

int syncutil::Condition::wait_on(const uint32_t val) {
    int err;
    while (futex.load(std::memory_order_relaxed) == val) {
        if (err = futex_wait(val)) {
            return err;
        }
    }
    return 0;
}

int syncutil::Condition::set_val(const uint32_t val, const bool wakeup) {
    futex.store(val, std::memory_order_release);
    // race condition can occur here, as user may overwrite this value
    // this is fine in all our usecases.
    if (wakeup) {
        return futex_wake();
    }
    return 0;

}

