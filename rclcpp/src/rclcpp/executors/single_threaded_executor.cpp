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

//using rclcpp::executors::SingleThreadedExecutor;

namespace rclcpp
{

namespace executors
{

// Define and initialize the static member variable
rclcpp::sched::SchedAttr rclcpp::executors::ThreadData::idle_sched_attr = {};

struct t_eventData {
    syncutil::Condition* signal_scheduler_ptr;
};

void handler(int sig, siginfo_t *si, void *uc) {
  UNUSED(sig);
  UNUSED(uc);
  t_eventData *data = (t_eventData *)si->_sifields._rt.si_sigval.sival_ptr;
  data->signal_scheduler_ptr->set_val(1, true);
}

SingleThreadedExecutor::SingleThreadedExecutor(const rclcpp::ExecutorOptions & options, const int worker_pool_size)
: rclcpp::Executor(options) 
{
  ThreadData::idle_sched_attr.sched_policy = SCHED_FIFO;
  ThreadData::idle_sched_attr.sched_priority = 1;

  CPU_ZERO(&(this->cpuset));
  CPU_SET(10, &(this->cpuset));
  CPU_SET(11, &(this->cpuset));
  CPU_SET(12, &(this->cpuset));
  CPU_SET(13, &(this->cpuset));
  for (int i = 0; i < worker_pool_size; i++)
  {
    create_thread();
  }
}

inline void SingleThreadedExecutor::worker_thread_loop(rclcpp::executors::ThreadData& thread_data) {
  while (true) {
    thread_data.is_busy.wait_on(0);
    std::cout << "pthread " << thread_data.pthread_id << " executing executable 0x" << &(thread_data.any_exec) << std::endl;
    this->execute_executable(thread_data.any_exec, thread_data.callbackInputs);
    thread_data.is_busy.set_val(0, false);
    this->idle_threads.push(&thread_data);
    thread_data.sched_attr = nullptr;
    syscall_sched_setattr(0, &ThreadData::idle_sched_attr);
  }
}

void SingleThreadedExecutor::worker_thread_func(const uint32_t worker_id) {
  rclcpp::executors::ThreadData thread_data(worker_id);
  this->idle_threads.push(&thread_data);
  worker_thread_loop(thread_data);
}

void SingleThreadedExecutor::worker_thread_func(AnyExecutable any_exec, CallbackInputs data, rclcpp::sched::SchedAttr* input_sched_attr, const uint32_t worker_id) {
  rclcpp::executors::ThreadData thread_data(std::move(any_exec), std::move(data), input_sched_attr, worker_id);
  worker_thread_loop(thread_data);
}

bool SingleThreadedExecutor::get_next_ready_executable_from_map(
  AnyExecutable & any_executable,
  const rclcpp::memory_strategy::MemoryStrategy::WeakCallbackGroupsToNodesMap &
  weak_groups_to_nodes)
{
  // TRACEPOINT(rclcpp_executor_get_next_ready);
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
  //   if (any_executable.callback_group && any_executable.callback_group->type() == 
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

static inline rclcpp::sched::SchedAttr* get_sched_attr(const rclcpp::AnyExecutable& any_exec) {
  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );
  if (any_exec.subscription) {
    return &(any_exec.subscription->sched_attr);
  }
  if (any_exec.timer) {
    return &(any_exec.timer->sched_attr);
  }
  if (any_exec.service) {
    return &(any_exec.service->sched_attr);
  }
  if (any_exec.client) {
    return &(any_exec.client->sched_attr);
  }
  if (any_exec.waitable) {
    return &(any_exec.waitable->sched_attr);
  }

  assert(false);
  return nullptr;
}

static inline rclcpp::sched::edf_sched_entity* get_sched_entity(const rclcpp::AnyExecutable& any_exec) {
  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );
  if (any_exec.subscription) {
    return &(any_exec.subscription->sched_entity);
  }
  if (any_exec.timer) {
    return &(any_exec.timer->sched_entity);
  }
  if (any_exec.service) {
    return &(any_exec.service->sched_entity);
  }
  if (any_exec.client) {
    return &(any_exec.client->sched_entity);
  }
  if (any_exec.waitable) {
    return &(any_exec.waitable->sched_entity);
  }

  assert(false);
  return nullptr;
}

static bool take_and_do_error_handling(const char *action_description,
                                       const char *topic_or_service_name,
                                       std::function<bool()> take_action) {
  bool taken = false;
  try {
    taken = take_action();
  } catch (const rclcpp::exceptions::RCLError &rcl_error) {
    RCLCPP_ERROR(rclcpp::get_logger("rclcpp"),
                 "executor %s '%s' unexpectedly failed: %s", action_description,
                 topic_or_service_name, rcl_error.what());
  }
  if (!taken) {
    // Message or Service was not taken for some reason.
    // Note that this can be normal, if the underlying middleware needs to
    // interrupt wait spuriously it is allowed.
    // So in that case the executor cannot tell the difference in a
    // spurious wake up and an entity actually having data until trying
    // to take the data.
    RCLCPP_DEBUG(rclcpp::get_logger("rclcpp"),
                 "executor %s '%s' failed to take anything", action_description,
                 topic_or_service_name);
  }

  return taken;
}

static bool take_subscription(rclcpp::SubscriptionBase::SharedPtr subscription, rclcpp::executors::SubscriptionHandlerInputs &data) 
{
  data.messageInfo.get_rmw_message_info().from_intra_process = false;

  bool taken = false;

  // This is the case where a copy of the serialized message is taken from
  // the middleware via inter-process communication.
  if (subscription->is_serialized()) 
  {
    data.message.serializedMessage = subscription->create_serialized_message();
    taken = take_and_do_error_handling(
        "taking a serialized message from topic",
        subscription->get_topic_name(),
        [&]() {
          return subscription->take_serialized(
              *(data.message.serializedMessage.get()), data.messageInfo);
        }
    );
  }

  // This is the case where a loaned message is taken from the middleware via
  // inter-process communication, given to the user for their callback,
  // and then returned.
  else if (subscription->can_loan_messages()) 
  {
    data.message.loanedMessage = nullptr;

    taken = take_and_do_error_handling(
        "taking a loaned message from topic", subscription->get_topic_name(),
        [&]() {
          rcl_ret_t ret = rcl_take_loaned_message(
              subscription->get_subscription_handle().get(),
              &(data.message.loanedMessage),
              &(data.messageInfo.get_rmw_message_info()), nullptr);
          if (RCL_RET_SUBSCRIPTION_TAKE_FAILED == ret) {
            return false;
          } else if (RCL_RET_OK != ret) {
            rclcpp::exceptions::throw_from_rcl_error(ret);
          }
          return true;
        }
      );
  }

  // This case is taking a copy of the message data from the middleware via
  // inter-process communication.
  else 
  {
    data.message.copiedMessage = subscription->create_message();

    taken = take_and_do_error_handling(
        "taking a message from topic", subscription->get_topic_name(),
        [&]() {
          return subscription->take_type_erased(
              data.message.copiedMessage.get(), data.messageInfo);
        }
      );
  }

  return taken;
}

static void exec_subscription(rclcpp::SubscriptionBase::SharedPtr subscription, rclcpp::executors::SubscriptionHandlerInputs &data) 
{
  int not_param_evt = strcmp("/parameter_events", subscription->get_topic_name());

    if (subscription->is_serialized()) 
  {
    if (not_param_evt)
    {
      assert(data.message.serializedMessage); 
    }

    subscription->handle_serialized_message(data.message.serializedMessage, data.messageInfo);
    subscription->return_serialized_message(data.message.serializedMessage);
  }

  else if (subscription->can_loan_messages()) 
  {
    // TODO: figure out if loanedMessage should always be non-null
    if (not_param_evt)
    {
      assert(data.message.loanedMessage); 
    }

    subscription->handle_loaned_message(data.message.loanedMessage, data.messageInfo);
    if (data.message.loanedMessage != nullptr) 
    {
      rcl_ret_t ret = rcl_return_loaned_message_from_subscription(subscription->get_subscription_handle().get(), data.message.loanedMessage);
      if (RCL_RET_OK != ret) {
        RCLCPP_ERROR(
          rclcpp::get_logger("rclcpp"),
          "rcl_return_loaned_message_from_subscription() failed for subscription on topic '%s': %s",
          subscription->get_topic_name(), rcl_get_error_string().str);
      }
      data.message.loanedMessage = nullptr;
    }
  }

  else 
  {
    if (not_param_evt)
    {
      assert(data.message.copiedMessage); 
    }

    subscription->handle_message(data.message.copiedMessage, data.messageInfo);
    subscription->return_message(data.message.copiedMessage);
  }
}

static bool take_service(rclcpp::ServiceBase::SharedPtr service, rclcpp::executors::ServiceHandlerInputs &data) 
{
  data.requestHeader = service->create_request_header();
  data.request = service->create_request();
  bool taken = take_and_do_error_handling(
      "taking a service server request from service",
      service->get_service_name(),
      [&]() {return service->take_type_erased_request(data.request.get(), *(data.requestHeader));
      });

  return taken;
}

static bool take_client(rclcpp::ClientBase::SharedPtr client, rclcpp::executors::ClientHandlerInputs &data)
{
  data.requestHeader = client->create_request_header();
  data.response = client->create_response();
  bool taken = take_and_do_error_handling(
    "taking a service client response from service",
    client->get_service_name(),
    [&]() {return client->take_type_erased_response(data.response.get(), *(data.requestHeader));});

  return taken;
}

void SingleThreadedExecutor::execute_executable(AnyExecutable any_exec, CallbackInputs callbackInputs) {
  if (any_exec.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    any_exec.callback_group->callback_group_mutex.lock();
  }

  if (any_exec.timer)
  {
    execute_timer(any_exec.timer);
  }
  else if (any_exec.subscription)
  {
    exec_subscription(any_exec.subscription, callbackInputs.subscriptionInput);
  }
  else if (any_exec.service)
  {
    any_exec.service->handle_request(callbackInputs.serviceInput.requestHeader,
                                     callbackInputs.serviceInput.request);
  } 
  else if (any_exec.client) 
  {
    any_exec.client->handle_response(callbackInputs.clientInput.requestHeader,
                                     callbackInputs.clientInput.response);
  } else if (any_exec.waitable) {
    any_exec.waitable->execute(any_exec.data);
  }

  if (any_exec.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    any_exec.callback_group->callback_group_mutex.unlock();
  }
  any_exec.callback_group.reset();
}

void SingleThreadedExecutor::create_thread(const uint32_t worker_id) {
  std::thread new_thread(
    std::bind(static_cast<void (SingleThreadedExecutor::*)()>(&SingleThreadedExecutor::worker_thread_func), this, const uint32_t worker_id));

  if (sched_setaffinity(sched::get_pid(new_thread.native_handle()),
                        sizeof(cpu_set_t), &(this->cpuset)) == -1) {
    std::cerr << "Error setting CPU affinity: " << strerror(errno) << std::endl;
    exit(1);
  }
  new_thread.detach();
}

void SingleThreadedExecutor::create_thread(const AnyExecutable any_exec, CallbackInputs data, const uint32_t worker_id) {
  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );

  auto sched_entity = get_sched_entity(any_exec);
  auto attr = get_sched_attr(any_exec);
  const std::string nodeName = any_exec.node_base->get_name();

  try
  {
    //std::thread new_thread(std::bind(&SingleThreadedExecutor::worker_thread_func, this, std::move(any_exec), std::move(data), attr));
    std::thread new_thread(
      std::bind(static_cast<void (SingleThreadedExecutor::*)(AnyExecutable, CallbackInputs, sched::SchedAttr*)>(&SingleThreadedExecutor::worker_thread_func),
       this, std::move(any_exec), std::move(data), attr, const uint32_t worker_id)
      );

    if (sched_setaffinity(sched::get_pid(new_thread.native_handle()), sizeof(cpu_set_t), &(this->cpuset)) == -1) {
      std::cerr << "Error setting CPU affinity: " << strerror(errno) << std::endl;
      exit(1);
    }

    if (sched_entity->edf_attr) {
      if (sched_entity->is_source) {
        //std::cout << "this is a source" << std::endl;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        //std::cout << "time in sec: " << now.tv_sec << std::endl;
        sched_entity->edf_attr->abs_deadline = (uint64_t) now.tv_sec * SEC_IN_NSEC + now.tv_nsec + sched_entity->relative_deadline;
      }
      //std::cout << "abs deadline is: " << sched_entity->edf_attr->abs_deadline << std::endl;
      sched::update_deadline(new_thread.native_handle(), sched_entity->edf_attr);
    } else {
      sched::syscall_sched_setattr(sched::get_pid(new_thread.native_handle()), attr);
    }
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

bool SingleThreadedExecutor::take_message(const AnyExecutable& any_exec, CallbackInputs& inputs)
{
  bool taken = false;
  if (any_exec.subscription)
	{
    taken = take_subscription(any_exec.subscription, inputs.subscriptionInput);
		assert(taken);
    //assert(inputs.subscriptionInput.messageInfo);
    if (any_exec.subscription->is_serialized()) 
    {
      assert(inputs.subscriptionInput.message.serializedMessage);
    }
    else if (any_exec.subscription->can_loan_messages()) 
    {
      assert(inputs.subscriptionInput.message.loanedMessage);
    }
    else
    {
      assert(inputs.subscriptionInput.message.copiedMessage);
    }
	}
  else if (any_exec.service)
  {
    taken = take_service(any_exec.service, inputs.serviceInput);
    assert(taken);
    assert(inputs.serviceInput.requestHeader);
    assert(inputs.serviceInput.request);
  }
  else if (any_exec.client)
  {
    taken = take_client(any_exec.client, inputs.clientInput);
    assert(taken);
    assert(inputs.clientInput.requestHeader);
    assert(inputs.clientInput.response);
  }
  else
  {
    taken = true;
  }

  return taken;
}

void SingleThreadedExecutor::assign_thread(ThreadData* thread, AnyExecutable any_exec, CallbackInputs callbackInputs)
{
  
  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );
  
  // Fill in the scheduling attributes of the worker thread taking the callback
  auto sched_entity = get_sched_entity(any_exec);
  thread->any_exec = std::move(any_exec);
  thread->callbackInputs = std::move(callbackInputs);
  thread->sched_attr = get_sched_attr(thread->any_exec);
  assert(thread->sched_attr);

  int res = 0;
  if (sched_entity->edf_attr) {
    if (sched_entity->is_source) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      sched_entity->edf_attr->abs_deadline = (uint64_t) now.tv_sec * SEC_IN_NSEC + now.tv_nsec + sched_entity->relative_deadline;
    }
    res = (sched::update_deadline(thread->pthread_id, sched_entity->edf_attr) == false);
  } else {
    res = sched::syscall_sched_setattr(thread->pid, thread->sched_attr);
  }
  
	if (res)
	{
		perror("Error while setting idle thread's sched_attr: ");
			RCLCPP_ERROR(
				rclcpp::get_logger("rclcpp"),
				"Error: Tried setting idle thread (pid=%d) with sched_attr, but got return code %d",
				thread->pid,
				res
			);

    assert(false);
	}

  thread->is_busy.set_val(1, true);
}

void SingleThreadedExecutor::dispatch(AnyExecutable any_exec) {

  if (
    any_exec.subscription == nullptr
    && any_exec.timer  == nullptr
    && any_exec.service == nullptr
    && any_exec.client == nullptr 
    && any_exec.waitable == nullptr
  )
  {
    execute_any_executable(any_exec);
    return;
  }

  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );

  CallbackInputs callbackInput;

  // Takes message off waitset if subscription, service, or client. A nop for timers and waitables.
	take_message(any_exec, callbackInput);

  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );

  ThreadData* idle_thread = idle_threads.pop();

  // If there are no idle work threads, then make a new one
  if (idle_thread == nullptr) {
    create_thread(std::move(any_exec), std::move(callbackInput));
    return;
  }

  assert(
    any_exec.subscription
    || any_exec.timer
    || any_exec.service
    || any_exec.client
    || any_exec.waitable
  );

  // Assign new callback to the idle thread
  assign_thread(idle_thread, std::move(any_exec), std::move(callbackInput));
}

void SingleThreadedExecutor::set_cpuset(const cpu_set_t new_cpuset)
{
  memcpy(&(this->cpuset), &new_cpuset, sizeof(cpu_set_t));
  this->cpuset = new_cpuset;
  assert(CPU_EQUAL(&new_cpuset, &(this->cpuset)));
}

void
SingleThreadedExecutor::spin() {
  printf("Spinning\n");
  if (spinning.exchange(true)) {
    throw std::runtime_error("spin() called while already spinning");
  }
  // char* core_count = getenv("ROS_CORE_COUNT");

  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false););
  int period_ns;
  char* method = getenv("ROS_SCHED_METHOD");
  char* period_str = getenv("ROS_SCHED_PERIOD");
	
	for (int i = 0; i < 50; i++)
	{
    printf("Warming up, iter: %d\n", i);
		AnyExecutable executable;
		bool success = get_next_executable(executable, std::chrono::nanoseconds(500000));
		if (success)
		{
      printf("success at iter: %d\n", i);
			execute_any_executable(executable);
		}	
	}
  std::cout << "Warmup complete" << std::endl;
  if (period_str == nullptr) {
    period_ns = DEFAULT_INTERVAL;
  } else {
    period_ns = atoi(period_str);
    assert(period_ns < SEC_IN_NSEC);
  }
  // if (method == nullptr || strcmp(method, "DEADLINE") == 0) {
  //   spin_deadline(period_ns);
  // } 
  if (method == nullptr || strcmp(method, "SLEEP") == 0) {
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

// void next_time(int period_ns, struct timespec& cur_time) {
//   assert(clock_gettime(CLOCK_MONOTONIC, &cur_time) == 0);
//   cur_time.tv_nsec -= cur_time.tv_nsec % period_ns - period_ns;
//   cur_time.tv_sec += cur_time.tv_nsec % SEC_IN_NSEC;
//   cur_time.tv_nsec %= SEC_IN_NSEC;
//   return;
// }

void
SingleThreadedExecutor::spin_sleep(int period_ns)
{
  std::cout << "In spin_sleep" << std::endl;

  sched::SchedAttr attr;
  attr.sched_policy = SCHED_FIFO;
  attr.sched_priority = 99;
  assert(sched::syscall_sched_setattr(gettid(), &attr) == 0);

  // Setup timer so it sleeps until the next period point
  struct timespec period_point;
  // int flags = TIMER_ABSTIME;
  // int err = 0;
  assert(clock_gettime(CLOCK_MONOTONIC, &period_point) == 0);
  inc_period(period_point, period_ns);

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
		// std::cout << "Wokeup at " << wake_time_actual.tv_sec * 1000'000'000 + wake_time_actual.tv_nsec << std::endl;

    this->schedule();

  	r = clock_gettime(CLOCK_MONOTONIC, &period_point);
		assert(r == 0);
		// struct timespec curr_time = period_point;
    inc_period(period_point, period_ns);

		//std::cout << "Sleeping at " << curr_time.tv_sec * 1000'000'000 + curr_time.tv_nsec << "\n";
		//std::cout << "Will wakeup at " << period_point.tv_sec * 1000'000'000 + period_point.tv_nsec << std::endl;

    // while (true) {
    //   rclcpp::AnyExecutable executable;
    //   if (!get_next_executable(executable)) {
    //     break;
    //   }
    //   assign_or_create(std::move(executable));
    // }
  }
}

// void
// SingleThreadedExecutor::spin_deadline(int period_ns)
// {
//   std::cout << "In spin_deadline" << std::endl;

//   for (int i = 0; i < 500; i++)
//   {
//     create_idle_thread();
//   }
  

//   sched::SchedAttr attr;
//   attr.sched_policy = SCHED_DEADLINE;
//   attr.sched_priority = 0;
//   attr.sched_period = period_ns;
//   attr.sched_runtime = period_ns;
//   attr.sched_deadline = period_ns;
// 	attr.sched_flags |= 0x04;
//   sched::syscall_sched_setattr(gettid(), &attr);
//   while (rclcpp::ok(this->context_) && spinning.load()) {
//     this->schedule();
//     sched_yield();
//   }
// }

void SingleThreadedExecutor::schedule() {
  // TRACEPOINT(rclcpp_schedule_entry);
  int num_cb_dispatched = 0;
  rclcpp::AnyExecutable executable;
  if (!get_next_executable(executable, std::chrono::nanoseconds::zero())) {
    // TRACEPOINT(rclcpp_schedule_exit, 0);
    return;
  }
  
  dispatch(std::move(executable));
  
  num_cb_dispatched++;
  while (true) {
    rclcpp::AnyExecutable ready_executable;
    if (!get_next_ready_executable(ready_executable)) {
      // TRACEPOINT(rclcpp_schedule_exit, num_cb_dispatched);
      return;
    }

    dispatch(std::move(ready_executable));
    num_cb_dispatched++;
  }
  // TRACEPOINT(rclcpp_schedule_exit, num_cb_dispatched);
}

}
}