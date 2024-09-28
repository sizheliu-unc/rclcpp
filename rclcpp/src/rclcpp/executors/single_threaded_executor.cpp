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

#include <pthread.h>
#include <stdio.h>

#include "rclcpp/callback_group.hpp"
#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/any_executable.hpp"

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

void* thread_start(void* pthread_arg) {
  printf("thread start!");
  rclcpp::executors::PthreadArg* arg = (rclcpp::executors::PthreadArg*) pthread_arg;
  auto executor = arg->executor;
  rclcpp::executors::ThreadData thread_data;
  thread_data.any_exec = arg->any_exec;
  thread_data.is_busy.set_val(1, false);
  delete arg;
  while (true) {
    thread_data.is_busy.wait_on(0);
    executor->execute_executable(thread_data.any_exec);
    thread_data.is_busy.set_val(0, false);
    executor->idle_threads.push(&thread_data);
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

bool
SingleThreadedExecutor::get_next_ready_executable(AnyExecutable & any_executable)
{
  TRACETOOLS_TRACEPOINT(rclcpp_executor_get_next_ready);

  bool valid_executable = false;

  if (!wait_result_.has_value() || wait_result_->kind() != rclcpp::WaitResultKind::Ready) {
    return false;
  }

  if (!valid_executable) {
    size_t current_timer_index = 0;
    while (true) {
      auto [timer, timer_index] = wait_result_->peek_next_ready_timer(current_timer_index);
      if (nullptr == timer) {
        break;
      }
      current_timer_index = timer_index;
      auto entity_iter = current_collection_.timers.find(timer->get_timer_handle().get());
      if (entity_iter != current_collection_.timers.end()) {
        auto callback_group = entity_iter->second.callback_group.lock();
        if (!callback_group || !callback_group->can_be_taken_from()) {
          current_timer_index++;
          continue;
        }
        // At this point the timer is either ready for execution or was perhaps
        // it was canceled, based on the result of call(), but either way it
        // should not be checked again from peek_next_ready_timer(), so clear
        // it from the wait result.
        wait_result_->clear_timer_with_index(current_timer_index);
        // Check that the timer should be called still, i.e. it wasn't canceled.
        any_executable.data = timer->call();
        if (!any_executable.data) {
          current_timer_index++;
          continue;
        }
        any_executable.timer = timer;
        any_executable.callback_group = callback_group;
        valid_executable = true;
        break;
      }
      current_timer_index++;
    }
  }

  if (!valid_executable) {
    while (auto subscription = wait_result_->next_ready_subscription()) {
      auto entity_iter = current_collection_.subscriptions.find(
        subscription->get_subscription_handle().get());
      if (entity_iter != current_collection_.subscriptions.end()) {
        auto callback_group = entity_iter->second.callback_group.lock();
        if (!callback_group || !callback_group->can_be_taken_from()) {
          continue;
        }
        any_executable.subscription = subscription;
        any_executable.callback_group = callback_group;
        valid_executable = true;
        break;
      }
    }
  }

  if (!valid_executable) {
    while (auto service = wait_result_->next_ready_service()) {
      auto entity_iter = current_collection_.services.find(service->get_service_handle().get());
      if (entity_iter != current_collection_.services.end()) {
        auto callback_group = entity_iter->second.callback_group.lock();
        if (!callback_group || !callback_group->can_be_taken_from()) {
          continue;
        }
        any_executable.service = service;
        any_executable.callback_group = callback_group;
        valid_executable = true;
        break;
      }
    }
  }

  if (!valid_executable) {
    while (auto client = wait_result_->next_ready_client()) {
      auto entity_iter = current_collection_.clients.find(client->get_client_handle().get());
      if (entity_iter != current_collection_.clients.end()) {
        auto callback_group = entity_iter->second.callback_group.lock();
        if (!callback_group || !callback_group->can_be_taken_from()) {
          continue;
        }
        any_executable.client = client;
        any_executable.callback_group = callback_group;
        valid_executable = true;
        break;
      }
    }
  }

  if (!valid_executable) {
    while (auto waitable = wait_result_->next_ready_waitable()) {
      auto entity_iter = current_collection_.waitables.find(waitable.get());
      if (entity_iter != current_collection_.waitables.end()) {
        auto callback_group = entity_iter->second.callback_group.lock();
        if (!callback_group || !callback_group->can_be_taken_from()) {
          continue;
        }
        any_executable.waitable = waitable;
        any_executable.callback_group = callback_group;
        any_executable.data = waitable->take_data();
        valid_executable = true;
        break;
      }
    }
  }
  return valid_executable;
}


inline void SingleThreadedExecutor::create_thread(rclcpp::AnyExecutable any_exec) {
    PthreadArg* pthread_arg= new PthreadArg(this, any_exec);
    pthread_t pid;
    pthread_create(&pid, NULL, thread_start, (void*) pthread_arg);
    printf("thread created!\n");
    pthread_detach(pid);
    printf("thread detached!\n");
}



void SingleThreadedExecutor::assign_or_create(AnyExecutable any_exec) {
  auto idle_thread = idle_threads.pop();
  if (idle_thread == nullptr) {
    create_thread(any_exec);
    return;
  }
  idle_thread->any_exec = any_exec;
  idle_thread->is_busy.set_val(1, true);
}


SingleThreadedExecutor::SingleThreadedExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options) {}

SingleThreadedExecutor::~SingleThreadedExecutor() {}

void
SingleThreadedExecutor::spin()
{
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCPPUTILS_SCOPE_EXIT(wait_result_.reset();this->spinning.store(false););

  // Clear any previous result and rebuild the waitset
  this->wait_result_.reset();
  this->entities_need_rebuild_ = true;
  timer_t timerId = 0;
  t_eventData eventData = {&signal_scheduler};
  union sigval sigv; 
  sigv.sival_ptr = &eventData;
  struct sigevent sev = {};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGRTMIN;
  sev.sigev_value = sigv;
  /* specifies the action when receiving a signal */
  struct sigaction sa = {};

  /* specify start delay and interval */
  struct itimerspec its = {};
  struct timespec it_interval = {};
  struct timespec it_value = {};
  it_interval.tv_nsec = 50'000'000;
  it_value.tv_nsec = 50'000'000;
  its.it_interval = it_interval;
  its.it_value = it_value;                        

  printf("Signal Interrupt Timer - thread-id: %d\n", gettid());

  /* create timer */
  if (timer_create(CLOCK_MONOTONIC, &sev, &timerId)){
      return;
  }

  sa.sa_flags = SA_SIGINFO;
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

void SingleThreadedExecutor::schedule() {
  this->signal_scheduler.wait_on(0);
  rclcpp::AnyExecutable executable;
  while (get_next_executable(executable)) {
    printf("Executable available, assigning to threads.\n");
    assign_or_create(executable);
  }
  printf("No more available executable.\n");
  this->signal_scheduler.set_val(0, false);
}
