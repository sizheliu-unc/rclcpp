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

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <signal.h>
#include <string>
#include <unordered_map>
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
  std::atomic<int64_t>* period_ptr;
  std::atomic<int64_t>* hold_until_ptr;  
  std::atomic<int64_t>* delay_next_ptr;   
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

  /**
   * \param name The name of the timer
   * \param period_ns The period in nanoseconds
   */
  RCLCPP_PUBLIC
  void
  set_timer_period(const std::string & name, int64_t period_ns);

  /**
   * \param name The name of the timer
   * \return The period in nanoseconds, or 0 if not found
   */
  RCLCPP_PUBLIC
  int64_t
  get_timer_period(const std::string & name) const;

  /**
   * \param config Map of timer names to periods in nanoseconds
   */
  RCLCPP_PUBLIC
  void
  set_timer_period_config(const std::unordered_map<std::string, int64_t> & config);

  /**
   * \param name The name of the timer
   * \param hold_until_ns Absolute CLOCK_MONOTONIC ns until which timer is held; 0 = clear
   */
  RCLCPP_PUBLIC
  void
  set_timer_hold_until(const std::string & name, int64_t hold_until_ns);

  /**
   * \param name The name of the timer
   * \param delay_ns Relative ns delay applied to the next fire; 0 = inactive
   */
  RCLCPP_PUBLIC
  void
  set_timer_delay_next(const std::string & name, int64_t delay_ns);
protected:
  virtual void
  apply_chain_priorities();

  /// Container for named callback entities collected from registered nodes.
  /// groups_by_name is passed to ChainPriorityAllocator::allocate().
  /// entities_by_name is used to read/write sched_attr on individual callbacks.
  struct NamedEntities {
    std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> groups_by_name;
    std::unordered_map<std::string, std::shared_ptr<rclcpp::sched::SchedBase>> entities_by_name;
  };

  /// Collect all named callback entities from registered nodes.
  NamedEntities
  collect_named_entities();

  /// Copy entity's existing sched_attr and apply only new_policy and new_priority.
  /// This preserves all other sched_attr fields (flags, nice, runtime, etc.).
  static void
  apply_sched_attr_to_entity(
    const std::shared_ptr<rclcpp::sched::SchedBase> & entity,
    uint32_t new_policy,
    uint32_t new_priority);

  std::shared_ptr<rclcpp::detail::ChainPriorityAllocator> chain_priority_allocator_;

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

  std::vector<PosixTimer*> timers;

  //maps timer name to atomic period (ns)
  std::unordered_map<std::string, std::atomic<int64_t>> timer_period_config_;
  // maps timer pointer to its atomic period
  std::unordered_map<rclcpp::TimerBase*, std::atomic<int64_t>*> timer_period_map_;

  // maps timer name to hold_until (absolute CLOCK_MONOTONIC ns; 0 = inactive)
  std::unordered_map<std::string, std::atomic<int64_t>> timer_hold_until_config_;
  // maps timer name to delay_next (relative ns; 0 = inactive)
  std::unordered_map<std::string, std::atomic<int64_t>> timer_delay_next_config_;
};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__SINGLE_THREADED_EXECUTOR_HPP_
