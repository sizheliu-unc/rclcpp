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

#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/any_executable.hpp"

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
