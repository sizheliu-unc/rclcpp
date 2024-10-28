// Copyright 2015 Open Source Robotics Foundation, Inc.
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

#include "rcpputils/scope_exit.hpp"

#include <cassert>
#include <pthread.h>
#include <stdio.h>
#include <thread>
#include <signal.h>
#include <sys/syscall.h>
#include <sched.h>

#include "rclcpp/callback_group.hpp"

#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/any_executable.hpp"
#include "rclcpp/sched_base.hpp"

#include "tracetools/tracetools.h"

#define UNUSED(expr) do { (void)(expr); } while (0)

using rclcpp::executors::SingleThreadedExecutor;


struct t_eventData {
    syncutil::Condition* signal_scheduler_ptr;
};

void handler(int sig, siginfo_t *si, void *uc) {
  UNUSED(sig);
  UNUSED(uc);
  t_eventData *data = (t_eventData *) si->_sifields._rt.si_sigval.sival_ptr;
  data->signal_scheduler_ptr->set_val(1, true);
}

void
SingleThreadedExecutor::thread_start(AnyExecutable any_exec, rclcpp::sched::SchedAttr* sched_attr) {
  rclcpp::executors::ThreadData thread_data(std::move(any_exec));
  thread_data.is_busy.set_val(1, false);
  pthread_t self = pthread_self();
  thread_data.pid = sched::get_pid(self);
  thread_data.sched_attr = sched_attr;
  while (true) {
    thread_data.is_busy.wait_on(0);
    this->execute_executable(thread_data.any_exec);
    thread_data.is_busy.set_val(0, false);
    this->idle_threads.push(&thread_data);
  }
}

void SingleThreadedExecutor::execute_executable(AnyExecutable any_exec) {
  if (any_exec.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    any_exec.callback_group->callback_group_mutex.lock();
    execute_any_executable(any_exec);
    any_exec.callback_group->callback_group_mutex.unlock();
    return;
  }
  execute_any_executable(any_exec);


}

bool SingleThreadedExecutor::get_next_ready_executable_from_map(
  AnyExecutable & any_executable,
  const rclcpp::memory_strategy::MemoryStrategy::WeakCallbackGroupsToNodesMap &
  weak_groups_to_nodes)
{
  TRACEPOINT(rclcpp_executor_get_next_ready);
  bool success = false;
  std::lock_guard<std::mutex> guard{mutex_};
  // Check the timers to see if there are any that are ready
  memory_strategy_->get_next_timer(any_executable, weak_groups_to_nodes);
  if (any_executable.timer) {
    success = true;
  }
  if (!success) {
    // Check the subscriptions to see if there are any that are ready
    memory_strategy_->get_next_subscription(any_executable, weak_groups_to_nodes);
    if (any_executable.subscription) {
      success = true;
    }
  }
  if (!success) {
    // Check the services to see if there are any that are ready
    memory_strategy_->get_next_service(any_executable, weak_groups_to_nodes);
    if (any_executable.service) {
      success = true;
    }
  }
  if (!success) {
    // Check the clients to see if there are any that are ready
    memory_strategy_->get_next_client(any_executable, weak_groups_to_nodes);
    if (any_executable.client) {
      success = true;
    }
  }
  if (!success) {
    // Check the waitables to see if there are any that are ready
    memory_strategy_->get_next_waitable(any_executable, weak_groups_to_nodes);
    if (any_executable.waitable) {
      any_executable.data = any_executable.waitable->take_data();
      success = true;
    }
  }
  // At this point any_executable should be valid with either a valid subscription
  // or a valid timer, or it should be a null shared_ptr
/*  if (success) {
    rclcpp::CallbackGroup::WeakPtr weak_group_ptr = any_executable.callback_group;
    auto iter = weak_groups_to_nodes.find(weak_group_ptr);
    if (iter == weak_groups_to_nodes.end()) {
      success = false;
    }
  }*/

  // If there is no ready executable, return false
  return success;
}

inline rclcpp::sched::SchedAttr* SingleThreadedExecutor::get_sched_attr(const AnyExecutable& any_exec) {
  if (any_exec.subscription != nullptr) {
    return &(any_exec.subscription->sched_attr);
  }
  if (any_exec.timer != nullptr ) {
    return &(any_exec.timer->sched_attr);
  }
  if (any_exec.service != nullptr) {
    return &(any_exec.service->sched_attr);
  }
  if (any_exec.client != nullptr) {
    return &(any_exec.client->sched_attr);
  }
  // this will never happen.
  assert(("Cannot execute executable!", false));
  return nullptr;
}

inline void SingleThreadedExecutor::create_thread(AnyExecutable any_exec) {
  auto attr = get_sched_attr(any_exec);
  /* here we use std::thread instead of pthread to make it clean. They involve the same
     underlying syscalls. */
  std::thread new_thread(&SingleThreadedExecutor::thread_start, this, std::move(any_exec), attr);
  sched::syscall_sched_setattr(sched::get_pid(new_thread.native_handle()), attr);
  new_thread.detach();

}



void SingleThreadedExecutor::assign_or_create(AnyExecutable any_exec) {
  auto idle_thread = idle_threads.pop();
  if (idle_thread == nullptr) {
    create_thread(std::move(any_exec));
    return;
  }
  auto attr = get_sched_attr(any_exec);
  idle_thread->any_exec = std::move(any_exec);
  idle_thread->sched_attr = attr;
  sched::syscall_sched_setattr(idle_thread->pid, attr);
  idle_thread->is_busy.set_val(1, true);
}



SingleThreadedExecutor::SingleThreadedExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options) {}

SingleThreadedExecutor::~SingleThreadedExecutor() {}

#ifdef USE_TIMER

void
SingleThreadedExecutor::spin()
{
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false); );
  pid_t cur_tid = gettid();

	timer_t timerId = 0;
  t_eventData eventData = {&signal_scheduler};
  union sigval sigv;
  sigv.sival_ptr = &eventData;
  struct sigevent sev = {};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGRTMIN;
  sev.sigev_value = sigv;
  sev._sigev_un._tid = cur_tid;
  /* specifies the action when receiving a signal */
  struct sigaction sa = {};

  /* specify start delay and interval */
  struct itimerspec its = {};
  struct timespec it_interval = {};
  struct timespec it_value = {};
  it_interval.tv_nsec = 500'000;
  it_value.tv_nsec = 500'000;
  its.it_interval = it_interval;
  its.it_value = it_value;

  printf("Signal Interrupt Timer - thread-id: %d\n", gettid());

  /* create timer */
  if (timer_create(CLOCK_MONOTONIC, &sev, &timerId)){
      return;
  }

  sa.sa_flags = (SA_SIGINFO | SA_RESTART);
  sa.sa_sigaction = handler;

  /* Initialize signal */
  sigemptyset(&sa.sa_mask);

  printf("Establishing handler for signal %d\n", SIGRTMIN);

  /* Register signal handler */
  if (sigaction(SIGRTMIN, &sa, NULL)) {
      return;
  }
  printf("starting timer\n");
  if(timer_settime(timerId, 0, &its, NULL)) {
      return;
  }
  while (rclcpp::ok(this->context_) && spinning.load()) {
    this->schedule();
  }
}

#else

void
SingleThreadedExecutor::spin()
{
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false); );
  sched::SchedAttr attr;
  attr.sched_policy = SCHED_DEADLINE;
  attr.sched_period = 500'000;
  attr.sched_runtime = 500'000;
  attr.sched_deadline = 500'000;
  syscall(SYS_sched_setattr, gettid(), attr, 0);
  while (rclcpp::ok(this->context_) && spinning.load()) {
    this->schedule();
    sched_yield();
  }
}

#endif

void SingleThreadedExecutor::schedule() {
  rclcpp::AnyExecutable executable;
  if (!get_next_executable(executable, std::chrono::nanoseconds::zero())) {
    return;
  }
  assign_or_create(std::move(executable));
  while (get_next_ready_executable(executable)) {
    assign_or_create(std::move(executable));
  }
}
