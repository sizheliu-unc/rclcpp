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

#ifndef RCLCPP__EXECUTORS__NO_EXECUTOR_HPP_
#define RCLCPP__EXECUTORS__NO_EXECUTOR_HPP_

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
#include "rclcpp/bundled_subscription.hpp"

namespace rclcpp
{
namespace detail
{
class ChainPriorityAllocator;
}  // namespace detail
}  // namespace rclcpp

#include "rclcpp/cond.hpp"
#include "rclcpp/stack.hpp"

namespace rclcpp
{
namespace executors
{
class NoExecutor;

enum ExecutableType {
  SUBSCRIPTION,
  SERVICE,
  CLIENT,
  WAITABLE,
  TIMER
};

struct Executable {
  ExecutableType type;
  CallbackGroup::SharedPtr callback_group;
  std::shared_ptr<rclcpp::BundledSubscription> subscription = nullptr;
  rclcpp::ServiceBase::SharedPtr service = nullptr;
  rclcpp::ClientBase::SharedPtr client = nullptr;
  rclcpp::Waitable::SharedPtr waitable = nullptr;
  rclcpp::TimerBase::SharedPtr timer = nullptr;
};

struct ThreadDataNoExec {
  syncutil::Condition is_busy;
  Executable executable;
  pthread_t pthread_id;
  pid_t pid;
  uint32_t current_prio;
};

struct PosixTimer {
  NoExecutor* executor;
  uint64_t period;
  rclcpp::TimerBase::SharedPtr timer;
  rclcpp::CallbackGroup::SharedPtr callback_group;
  timer_t timerid;
  int index;
};

/// Single-threaded executor implementation.
/**
 * This is the default executor created by rclcpp::spin.
 */
class NoExecutor : public rclcpp::Executor
{
using rclcpp::Executor::add_node;
using rclcpp::Executor::remove_node;
public:
  RCLCPP_SMART_PTR_DEFINITIONS(NoExecutor)

  /// Default constructor. See the default constructor for Executor.
  RCLCPP_PUBLIC
  explicit NoExecutor(
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions(),
    std::shared_ptr<rclcpp::detail::ChainPriorityAllocator> chain_priority_allocator = nullptr);

  /// Default destructor.
  RCLCPP_PUBLIC
  virtual ~NoExecutor();

  void 
  start();

  RCLCPP_PUBLIC
  void
  spin() override;


  void
  stop();

  void
  execute_executable(Executable &executable);

  syncutil::StackAtomic<ThreadDataNoExec> idle_threads;

  RCLCPP_PUBLIC
  void
  add_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify = true) override;

  RCLCPP_PUBLIC
  void
  remove_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify = true) override;

  bool started;

  static uint64_t 
  get_period_from_timer(const rclcpp::TimerBase::SharedPtr &timer);

  void
  assign_or_create(Executable &executable);

private:
  void 
  handle_subscription(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::SubscriptionBase::SharedPtr &subscription, size_t num_msgs);

  void
  handle_service(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::ServiceBase::SharedPtr &service, size_t num_msgs);
  
  void
  handle_client(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::ClientBase::SharedPtr &client, size_t num_msgs); 
  
  void
  handle_waitable(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::Waitable::SharedPtr &waitable, size_t num_msgs);

  void
  create_thread(Executable executable);
  void
  thread_start(Executable executable);

  void
  apply_chain_priorities();

  std::vector<PosixTimer*> timers;
  std::shared_ptr<rclcpp::detail::ChainPriorityAllocator> chain_priority_allocator_;
};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__SINGLE_THREADED_EXECUTOR_HPP_
