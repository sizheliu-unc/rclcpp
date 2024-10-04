#pragma once

#include <unistd.h>
#include <stdint.h>
#include <atomic>

namespace syncutil{

class Condition {

public:
    int wait_on(const uint32_t val);
    int set_val(const uint32_t val, const bool wakeup);

private:
    std::atomic<uint32_t> futex;
    int futex_wait(const uint32_t val);
    int futex_wake();
};
}
