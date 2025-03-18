// Copyright 2014 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef RCLCPP__EXECUTORS__SINGLE_THREADED_EXECUTOR_HPP_
#define RCLCPP__EXECUTORS__SINGLE_THREADED_EXECUTOR_HPP_

#include <rmw/rmw.h>

#include <cassert>
#include <cstdlib>
#include <memory>
#include <signal.h>
#include <vector>
#include <pthread.h>

#include "rclcpp/executor.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/memory_strategies.hpp"
#include "rclcpp/node.hpp"
#include "rclcpp/utilities.hpp"
#include "rclcpp/rate.hpp"
#include "rclcpp/sched_base.hpp"
#include "rclcpp/visibility_control.hpp"

#include "rclcpp/cond.hpp"
#include "rclcpp/stack.hpp"

namespace rclcpp
{
namespace executors
{

struct SubscriptionHandlerInputs {
  struct {
    std::shared_ptr<SerializedMessage> serializedMessage;
    void* loanedMessage;
    std::shared_ptr<void> copiedMessage;
  } message;

  rclcpp::MessageInfo messageInfo;
};

struct ServiceHandlerInputs {
  std::shared_ptr<void> request;
  std::shared_ptr<rmw_request_id_t> requestHeader;
};

struct ClientHandlerInputs {
  std::shared_ptr<void> response;
  std::shared_ptr<rmw_request_id_t> requestHeader;
};

struct CallbackInputs {
  SubscriptionHandlerInputs subscriptionInput;
  ServiceHandlerInputs serviceInput;
  ClientHandlerInputs clientInput;
};

struct ThreadData {
  syncutil::Condition is_busy;
  AnyExecutable any_exec;
  pthread_t pthread_id;
  pid_t pid;
  uint32_t worker_id;
  sched::SchedAttr* sched_attr;
  static sched::SchedAttr idle_sched_attr;
	CallbackInputs callbackInputs;

  ThreadData(uint32_t worker_id)
  {
    this->worker_id = worker_id;
    is_busy.set_val(0, false);
    const pthread_t self = pthread_self();
    pthread_id = self;
    pid = sched::get_pid(self);
    sched_attr = &idle_sched_attr;
    syscall_sched_setattr(0, &ThreadData::idle_sched_attr);
  }

  ThreadData(AnyExecutable any_exec, CallbackInputs callbackInputs, rclcpp::sched::SchedAttr* input_sched_attr)
    : any_exec(std::move(any_exec)), callbackInputs(std::move(callbackInputs), uint32_t worker_id) 
  {
    this->worker_id = worker_id;
    is_busy.set_val(1, false);
    const pthread_t self = pthread_self();
    pthread_id = self;
    pid = sched::get_pid(self);
    sched_attr = input_sched_attr;
  }
};

/// Single-threaded executor implementation.
/**
 * This is the default executor created by rclcpp::spin.
 */
class SingleThreadedExecutor : public rclcpp::Executor
{
public:
  RCLCPP_SMART_PTR_DEFINITIONS(SingleThreadedExecutor)

  /// Default constructor. See the default constructor for Executor.
  RCLCPP_PUBLIC
  explicit SingleThreadedExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    const int worker_pool_size = 500
  );

  /// Default destructor.
  RCLCPP_PUBLIC
  virtual ~SingleThreadedExecutor() {}

  /// Single-threaded implementation of spin.
  /**
   * This function will block until work comes in, execute it, and then repeat
   * the process until canceled.
   * It may be interrupt by a call to rclcpp::Executor::cancel() or by ctrl-c
   * if the associated context is configured to shutdown on SIGINT.
   * \throws std::runtime_error when spin() called while already spinning
   */
  RCLCPP_PUBLIC
  void
  spin() override;

  syncutil::StackAtomic<ThreadData> idle_threads;
  RCLCPP_PUBLIC

	bool get_next_ready_executable_from_map(
		AnyExecutable & any_executable,
		const rclcpp::memory_strategy::MemoryStrategy::WeakCallbackGroupsToNodesMap &
		weak_groups_to_nodes) override;

  RCLCPP_PUBLIC
  void set_cpuset(const cpu_set_t new_cpuset);

private:
  RCLCPP_DISABLE_COPY(SingleThreadedExecutor)
  void spin_timer(int period_ns);
  void spin_sleep(int period_ns);
  // void spin_deadline(int period_ns);
  syncutil::Condition signal_scheduler;
  cpu_set_t cpuset;
  bool take_message(const AnyExecutable& any_exec, CallbackInputs& inputs);
  void execute_executable(AnyExecutable any_exec, CallbackInputs callbackInputs);
  void schedule();
  void create_thread(const uint32_t worker_id);
  void create_thread(const AnyExecutable any_exec, CallbackInputs data, const uint32_t worker_id);
  void assign_thread(ThreadData* thread, AnyExecutable any_exec, CallbackInputs callbackInputs);
  void dispatch(AnyExecutable any_exec);
  void worker_thread_loop(rclcpp::executors::ThreadData& thread_data);
  void worker_thread_func(const uint32_t worker_id);
  void worker_thread_func(AnyExecutable any_exec, CallbackInputs callbackInputs, sched::SchedAttr* sched_attr, const uint32_t worker_id);
};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__SINGLE_THREADED_EXECUTOR_HPP_
