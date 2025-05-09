#include "rclcpp/sched_base.hpp"

namespace rclcpp {
namespace sched {
int PureEDF::pure_edf_fd = -1;
bool update_deadline(pthread_t pthread_id, PureEDF* edf_attr) {
    if (PureEDF::pure_edf_fd < 0) {
        std::cerr << "Pure EDF fd is invalid!" << std::endl;
        return false;
    }
    sched_param ext_param = {0};
    EDF_attr_struct attr;
    attr.pid = get_pid(pthread_id);
    attr.abs_deadline = edf_attr->abs_deadline;
    ssize_t bytes_written = write(PureEDF::pure_edf_fd, &attr, sizeof(EDF_attr_struct));
    //std::cout << "Wrote " << bytes_written << " bytes to pipe on fd " << PureEDF::pure_edf_fd << std::endl;
    if (bytes_written <= 0) {
        std::cerr << "Cannot update deadline for pid " << attr.pid << std::endl;
        return false;
    }
    pthread_setschedparam(pthread_id, 7, &ext_param);
    return bytes_written > 0;
}
void
SchedBase::set_sched_attr(const SchedAttr& sched_attr) {
    this->sched_attr = sched_attr;
}
void
SchedBase::set_edf_attr(PureEDF* edf_attr) {
    this->sched_entity.edf_attr = edf_attr;
}

void
SchedBase::set_edf_entity(const edf_sched_entity& sched_entity) {
    this->sched_entity = sched_entity;
}
};
};


