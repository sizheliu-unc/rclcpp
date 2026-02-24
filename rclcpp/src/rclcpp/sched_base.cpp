#include "rclcpp/sched_base.hpp"

namespace rclcpp {
namespace sched {
void
SchedBase::set_sched_attr(const SchedAttr& sched_attr) {
    this->sched_attr = sched_attr;
}
};
};


