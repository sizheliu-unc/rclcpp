#include "rclcpp/sched_base.hpp"

namespace rclcpp {
namespace sched {
int PureEDF::pure_edf_fd = -1;
bool update_deadline(pthread_t pthread_id, PureEDF* edf_attr) {
    if (PureEDF::pure_edf_fd < 0) {
        std::cout << "Pure EDF fd is invalid!" << std::endl;
        return false;
    }
    sched_param ext_param = {0};
    EDF_attr_struct attr;
    attr.pid = get_pid(pthread_id);
    attr.abs_deadline = edf_attr->abs_deadline;
    ssize_t bytes_written = write(PureEDF::pure_edf_fd, &attr, sizeof(EDF_attr_struct));
    std::cout << "Wrote " << bytes_written << " bytes to pipe on fd " << PureEDF::pure_edf_fd << std::endl;
    pthread_setschedparam(pthread_id, 7, &ext_param);
    return bytes_written > 0;
}

};
};


