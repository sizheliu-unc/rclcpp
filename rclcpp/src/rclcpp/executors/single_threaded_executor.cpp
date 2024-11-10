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
#include <unordered_set>
#include <sched.h>
#include <system_error>

#include "rclcpp/callback_group.hpp"

#include "rclcpp/executors/single_threaded_executor.hpp"
#include "rclcpp/any_executable.hpp"
#include "rclcpp/sched_base.hpp"

#include "tracetools/tracetools.h"

#define UNUSED(expr) do { (void)(expr); } while (0)
#define DEFAULT_INTERVAL 500'000

#define SEC_IN_NSEC 1'000'000'000

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
SingleThreadedExecutor::thread_start(AnyExecutable any_exec, std::shared_ptr<void>& message, rclcpp::MessageInfo* message_info, rclcpp::sched::SchedAttr* sched_attr) {
  TRACEPOINT(rclcpp_worker_thread_spawn);
  rclcpp::executors::ThreadData thread_data(std::move(any_exec));
	thread_data.message = message;
	thread_data.message_info = message_info;

  thread_data.is_busy.set_val(1, false);
  pthread_t self = pthread_self();
  thread_data.pid = sched::get_pid(self);
  thread_data.sched_attr = sched_attr;
  while (true) {
    TRACEPOINT(rclcpp_worker_thread_yield);
    thread_data.is_busy.wait_on(0);
    TRACEPOINT(rclcpp_worker_thread_resume);
    this->execute_executable(thread_data.any_exec, thread_data.message, thread_data.message_info);
    thread_data.is_busy.set_val(0, false);
    TRACEPOINT(rclcpp_idle_thread_stack_push);
    this->idle_threads.push(&thread_data);
  }
}

void SingleThreadedExecutor::execute_executable(AnyExecutable any_exec, std::shared_ptr<void>& message, rclcpp::MessageInfo* message_info) {
  if (any_exec.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    any_exec.callback_group->callback_group_mutex.lock();
    if (any_exec.subscription == nullptr)
    {
      execute_any_executable(any_exec);
    }
    else
    {
			if (strcmp("/parameter_events", any_exec.subscription->get_topic_name()))
			{
				assert(message);
				assert(message_info);
			}
			std::cout << "Execute subscriber to topic: " << any_exec.subscription->get_topic_name() << std::endl;
      any_exec.subscription->handle_message(message, *message_info);
      any_exec.subscription->return_message(message);
      delete message_info;
    }
    any_exec.callback_group->callback_group_mutex.unlock();
    any_exec.callback_group.reset();
    return;
  }


  if (any_exec.subscription == nullptr)
  {
    execute_any_executable(any_exec);
  }
  else
  {
    assert(message);
    assert(message_info);
    any_exec.subscription->handle_message(message, *message_info);
    any_exec.subscription->return_message(message);
    delete message_info;
  }

  // Clear the callback_group to prevent the AnyExecutable destructor from
  // resetting the callback group `can_be_taken_from`
  any_exec.callback_group.reset();


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
  if (success) {
    rclcpp::CallbackGroup::WeakPtr weak_group_ptr = any_executable.callback_group;
    auto iter = weak_groups_to_nodes.find(weak_group_ptr);
    if (iter == weak_groups_to_nodes.end()) {
			std::cout << "Couldn't find the callback group" << std::endl;
      success = false;
    }
  }

  // if (success) {
  //   // If it is valid, check to see if the group is mutually exclusive or
  //   // not, then mark it accordingly ..Check if the callback_group belongs to this executor
  //   if (any_executable.callback_group && any_executable.callback_group->type() == \
  //     CallbackGroupType::MutuallyExclusive)
  //   {
  //     // It should not have been taken otherwise
  //     assert(any_executable.callback_group->can_be_taken_from().load());
  //     // Set to false to indicate something is being run from this group
  //     // This is reset to true either when the any_executable is executed or when the
  //     // any_executable is destructued
  //     any_executable.callback_group->can_be_taken_from().store(false);
  //   }
  // }
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
  if (any_exec.waitable != nullptr) {
    return &(any_exec.waitable->sched_attr);
  }
  // this will never happen.
  assert(false);
  return nullptr;
}

static bool take_message(rclcpp::AnyExecutable& any_exec, std::shared_ptr<void>& message, rclcpp::MessageInfo** message_info_ptr)
{
	rclcpp::MessageInfo* message_info = new rclcpp::MessageInfo;
	message_info->get_rmw_message_info().from_intra_process = false;
	message = any_exec.subscription->create_message();
	assert(message);
	assert(message.get() != nullptr);
	bool taken = false;

	try {
		taken = any_exec.subscription->take_type_erased(message.get(), *message_info);
	} catch (const rclcpp::exceptions::RCLError & rcl_error) {
		RCLCPP_ERROR(
			rclcpp::get_logger("rclcpp"),
			"executor taking a message from topic '%s' unexpectedly failed: %s",
			any_exec.subscription->get_topic_name(),
			rcl_error.what());
	}

	if (!taken)
	{
		RCLCPP_DEBUG(
			rclcpp::get_logger("rclcpp"),
			"executor taking a message from topic '%s' failed to take anything",
			any_exec.subscription->get_topic_name());
		
		// No point spinning off a thread that won't have anything to work on
		any_exec.subscription->return_message(message);
		delete message_info;
		message_info = nullptr;
	}

	if (taken)
	{
		assert(message_info);
		assert(message.get() != nullptr);
	}

	*message_info_ptr = message_info;
	return taken;
}

inline void SingleThreadedExecutor::create_thread(AnyExecutable any_exec, std::shared_ptr<void>& message, rclcpp::MessageInfo* message_info) {
  auto attr = get_sched_attr(any_exec);
  /* here we use std::thread instead of pthread to make it clean. They involve the same
     underlying syscalls. */

  const std::string nodeName = any_exec.node_base->get_name();
	if (any_exec.subscription)
	{
		assert(message.get() != nullptr);
		assert(message_info != nullptr);
	}

  try
  {
    std::thread new_thread(std::bind(&SingleThreadedExecutor::thread_start, this, std::move(any_exec), message, message_info, attr));
    sched::syscall_sched_setattr(sched::get_pid(new_thread.native_handle()), attr);
    new_thread.detach();
  }
  catch(const std::system_error& e)
  {
      std::cout << "Caught system_error with code "
                    "[" << e.code() << "] meaning "
                    "[" << e.what() << "]\n";
      std::cout << "Failed to create thread for node " << nodeName << std::endl;
  }
}



void SingleThreadedExecutor::assign_or_create(AnyExecutable any_exec) {

	std::shared_ptr<void> message(nullptr);
	rclcpp::MessageInfo* message_info = nullptr;

	if (any_exec.subscription)
	{
		bool taken = take_message(any_exec, message, &message_info);	
		assert(taken);
		if (!taken)
		{
			return;
		}
		assert(message.get() != nullptr);
		assert(message_info != nullptr);
	}

  TRACEPOINT(rclcpp_idle_thread_stack_pop);
  auto idle_thread = idle_threads.pop();
  if (idle_thread == nullptr) {
    TRACEPOINT(rclcpp_create_worker_thread);
    create_thread(std::move(any_exec), message, message_info);
    return;
  }
  auto attr = get_sched_attr(any_exec);
	if (any_exec.waitable)
	{
			RCLCPP_INFO(
				rclcpp::get_logger("rclcpp"),
				"In a waitable");
	}
		
  idle_thread->any_exec = std::move(any_exec);

	idle_thread->message = message;
	idle_thread->message_info = message_info;

  idle_thread->sched_attr = attr;
  int res = sched::syscall_sched_setattr(idle_thread->pid, attr);
	assert(res == 0);
	if (attr->sched_priority == 99)
	{
		if (any_exec.subscription)
		{
			RCLCPP_ERROR(
				rclcpp::get_logger("rclcpp"),
				"Error: Node %s is trying to run a subscription CB from topic %s with priority 99",
				any_exec.node_base->get_name(),
				any_exec.subscription->get_topic_name());
		}
		else
		{
			RCLCPP_ERROR(
				rclcpp::get_logger("rclcpp"),
				"Error: Node %s is trying to run a non-subscription CB with priority 99",
				any_exec.node_base->get_name());
		}
	}
  TRACEPOINT(rclcpp_wake_worker_thread, idle_thread->pid);
  idle_thread->is_busy.set_val(1, true);
}



SingleThreadedExecutor::SingleThreadedExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options) {}

SingleThreadedExecutor::~SingleThreadedExecutor() {}

void
SingleThreadedExecutor::spin() {
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false); );
  int period_ns;
  char* method = getenv("ROS_SCHED_METHOD");
  char* period_str = getenv("ROS_SCHED_PERIOD");
	
	for (int i = 0; i < 50; i++)
	{
		AnyExecutable executable;
		bool success = get_next_executable(executable);
		if (success)
		{
			execute_any_executable(executable);
		}	
	}
  if (period_str == nullptr) {
    period_ns = DEFAULT_INTERVAL;
  } else {
    period_ns = atoi(period_str);
    assert(period_ns < SEC_IN_NSEC);
  }
  if (method == nullptr || strcmp(method, "DEADLINE") == 0) {
    spin_deadline(period_ns);
  } else if (strcmp(method, "SLEEP") == 0) {
    spin_sleep(period_ns);
  } else if (strcmp(method, "TIMER") == 0) {
    spin_timer(period_ns);
  } else {
    assert(false);
  }
}

void
SingleThreadedExecutor::spin_timer(int period_ns)
{
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
  it_interval.tv_nsec = period_ns;
  it_value.tv_nsec = period_ns;
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

void inc_period(struct timespec& period_time, int period_ns) 
{
  period_time.tv_nsec += period_ns;

  while (period_time.tv_nsec >= 1000000000) {
          /* timespec nsec overflow */
          period_time.tv_sec++;
          period_time.tv_nsec -= 1000000000;
  }
}

void next_time(int period_ns, struct timespec& cur_time) {
  assert(clock_gettime(CLOCK_MONOTONIC, &cur_time) == 0);
  cur_time.tv_nsec -= cur_time.tv_nsec % period_ns - period_ns;
  cur_time.tv_sec += cur_time.tv_nsec % SEC_IN_NSEC;
  cur_time.tv_nsec %= SEC_IN_NSEC;
  return;
}

void
SingleThreadedExecutor::spin_sleep(int period_ns)
{
  sched::SchedAttr attr;
  attr.sched_policy = SCHED_FIFO;
  attr.sched_priority = 99;
  assert(sched::syscall_sched_setattr(gettid(), &attr) == 0);

  // Setup timer so it sleeps until the next period point
  struct timespec period_point;
  int flags = TIMER_ABSTIME;
  int err = 0;
  assert(clock_gettime(CLOCK_MONOTONIC, &period_point) == 0);
  inc_period(period_point, period_ns);
	//next_time(period_ns, period_point);

	struct timespec wake_time_actual;
	wake_time_actual.tv_sec = 9;
	wake_time_actual.tv_nsec = 9;
  while (rclcpp::ok(this->context_) && spinning.load()) {

    // Sleep until next period
    //while((err = clock_nanosleep(CLOCK_MONOTONIC, flags, &period_point, NULL)) && errno == EINTR);
    //assert(err == 0);
		int err;
		do {
			// perform an absolute sleep until tsk->current_activation
			err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &period_point, NULL);
			// if err is nonzero, we might have woken up too early
		} while (err != 0 && errno == EINTR);
		assert(err == 0);

		
  	int r = clock_gettime(CLOCK_MONOTONIC, &wake_time_actual);
		assert(r == 0);
		//std::cout << "Wokeup at " << wake_time_actual.tv_sec * 1000'000'000 + wake_time_actual.tv_nsec << std::endl;

    this->schedule();

  	r = clock_gettime(CLOCK_MONOTONIC, &period_point);
		assert(r == 0);
		struct timespec curr_time = period_point;
    inc_period(period_point, period_ns);

		//std::cout << "Sleeping at " << curr_time.tv_sec * 1000'000'000 + curr_time.tv_nsec << "\n";
		//std::cout << "Will wakeup at " << period_point.tv_sec * 1000'000'000 + period_point.tv_nsec << std::endl;

		//next_time(period_ns, period_point);

    // while (true) {
    //   rclcpp::AnyExecutable executable;
    //   if (!get_next_executable(executable)) {
    //     break;
    //   }
    //   assign_or_create(std::move(executable));
    // }
  }
}

void
SingleThreadedExecutor::spin_deadline(int period_ns)
{
  sched::SchedAttr attr;
  attr.sched_policy = SCHED_DEADLINE;
  attr.sched_period = period_ns;
  attr.sched_runtime = period_ns;
  attr.sched_deadline = period_ns;
  sched::syscall_sched_setattr(gettid(), &attr);
  while (rclcpp::ok(this->context_) && spinning.load()) {
    this->schedule();
    sched_yield();
  }
}

void SingleThreadedExecutor::schedule() {
  TRACEPOINT(rclcpp_schedule_start);
  int num_cb_dispatched = 0;
  rclcpp::AnyExecutable executable;
  if (!get_next_executable(executable, std::chrono::nanoseconds::zero())) {
    TRACEPOINT(rclcpp_schedule_end, 0);
    return;
  }
  
  assign_or_create(std::move(executable));
  
  num_cb_dispatched++;
  while (true) {
    rclcpp::AnyExecutable ready_executable;
    if (!get_next_ready_executable(ready_executable)) {
      TRACEPOINT(rclcpp_schedule_end, num_cb_dispatched);
      return;
    }

    assign_or_create(std::move(ready_executable));
    num_cb_dispatched++;
  }
  TRACEPOINT(rclcpp_schedule_end, num_cb_dispatched);
}
