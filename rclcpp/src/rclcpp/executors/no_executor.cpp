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
#include <atomic>
#include <mutex>

#include "rclcpp/callback_group.hpp"

#include "rclcpp/executors/no_executor.hpp"
#include "rclcpp/sched_base.hpp"
#include "rclcpp/detail/chain_priority_allocator.hpp"
#include "rclcpp/logging.hpp"

#include "tracetools/tracetools.h"

#define UNUSED(expr) do { (void)(expr); } while (0)
#define SEC_IN_NSEC 1'000'000'000

using std::placeholders::_1;
using rclcpp::executors::NoExecutor;
using rclcpp::executors::Executable;
using rclcpp::executors::ExecutableType;
using rclcpp::executors::PosixTimer;

struct itimerspec unset_timer = {};
void 
handle_timer(int sig, siginfo_t *si, void *uc);

NoExecutor::NoExecutor(
  const rclcpp::ExecutorOptions & options,
  std::shared_ptr<rclcpp::detail::ChainPriorityAllocator> chain_priority_allocator)
: rclcpp::Executor(options),
  chain_priority_allocator_(std::move(chain_priority_allocator)) {
  started = false;
}

NoExecutor::~NoExecutor() {
  stop();
}

void
NoExecutor::start() {
  pid_t cur_tid = gettid();
  for (PosixTimer *timer: timers) {
    timer_t timerId = 0;
    union sigval sigv;
    sigv.sival_ptr = (void *) timer;
    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGRTMAX;
    sev.sigev_value = sigv;
    sev._sigev_un._tid = cur_tid;
    /* specifies the action when receiving a signal */
    assert(timer_create(CLOCK_MONOTONIC, &sev, &timerId) == 0);
    timer->timerid = timerId;
  }

  struct sigaction sa = {};
  sa.sa_flags = (SA_SIGINFO | SA_NODEFER | SA_RESTART);
  sa.sa_sigaction = handle_timer;
  sigemptyset(&sa.sa_mask);
  assert(sigaction(SIGRTMAX, &sa, NULL) == 0);
  
  started = true;
  for (PosixTimer *timer: timers) {
    /* specify start delay and interval */
    struct itimerspec its = {};
    struct timespec it_interval = {};
    struct timespec it_value = {};
    it_interval.tv_nsec = timer->period % SEC_IN_NSEC;
    it_interval.tv_sec = timer->period / SEC_IN_NSEC;
    it_value.tv_nsec = 50;
    it_value.tv_sec = 0;
    its.it_interval = it_interval;
    its.it_value = it_value;
    assert(timer_settime(timer->timerid, 0, &its, NULL) == 0);
  }
}

void
NoExecutor::stop() {
  started = false;
  for (PosixTimer *timer: timers) {
    assert(timer_settime(timer->timerid, 0, &unset_timer, NULL) == 0);
  }
}

void
NoExecutor::execute_executable(Executable &executable) {
  if (executable.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    executable.callback_group->callback_group_mutex.lock();
  }
  switch (executable.type)
  {
  case ExecutableType::SUBSCRIPTION:
    this->execute_subscription(executable.subscription);
    break;
  case ExecutableType::SERVICE:
    this->execute_service(executable.service);
    break;
  case ExecutableType::CLIENT:
    this->execute_client(executable.client);
    break;
  case ExecutableType::WAITABLE:
    {
      std::shared_ptr<void> data = executable.waitable->take_data();
      executable.waitable->execute(data);
      break;
    }
  case ExecutableType::TIMER:
    executable.timer->execute_callback();
  default:
    break;
  }
  if (executable.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    executable.callback_group->callback_group_mutex.unlock();
    executable.callback_group.reset();
  }
}

struct rcl_timer_ {
  void* unused[5];
  std::atomic_int_least64_t period;
};

uint64_t 
NoExecutor::get_period_from_timer(const rclcpp::TimerBase::SharedPtr &timer) {
  return ((rcl_timer_*) timer->get_timer_handle()->impl)->period;
}


void
NoExecutor::add_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify) {
  this->add_node(node_ptr->get_node_base_interface(), notify);
  node_ptr->for_each_callback_group([this](rclcpp::CallbackGroup::SharedPtr callback_group) {
    callback_group->collect_all_ptrs(
      [this, &callback_group](const rclcpp::SubscriptionBase::SharedPtr &subscription) {
        subscription->set_on_new_message_callback(std::bind(&NoExecutor::handle_subscription, this, callback_group, subscription, _1));
        subscription->set_on_new_intra_process_message_callback(std::bind(&NoExecutor::handle_subscription, this, callback_group, subscription, _1));
      },
      [this, &callback_group](const rclcpp::ServiceBase::SharedPtr &service) {
        service->set_on_new_request_callback(std::bind(&NoExecutor::handle_service, this, callback_group, service, _1));
      },
      [this, &callback_group](const rclcpp::ClientBase::SharedPtr &client) {
        client->set_on_new_response_callback(std::bind(&NoExecutor::handle_client, this, callback_group, client, _1));
      },
      [this, &callback_group](const rclcpp::TimerBase::SharedPtr &timer) {
        this->timers.push_back(new PosixTimer({this, get_period_from_timer(timer), timer, callback_group, 0, this->timers.size()}));
      },
      [this, &callback_group](const rclcpp::Waitable::SharedPtr &waitable) {
        waitable->set_on_ready_callback(std::bind(&NoExecutor::handle_waitable, this, callback_group, waitable, _1));
      }
    );
  });

  apply_chain_priorities();
}

void
NoExecutor::remove_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify)
{
  node_ptr->for_each_callback_group([&](rclcpp::CallbackGroup::SharedPtr callback_group) {
    callback_group->collect_all_ptrs(
      [](const rclcpp::SubscriptionBase::SharedPtr &subscription) {
        subscription->clear_on_new_message_callback();
        subscription->clear_on_new_intra_process_message_callback();
      },
      [](const rclcpp::ServiceBase::SharedPtr &service) {
        service->clear_on_new_request_callback();
      },
      [](const rclcpp::ClientBase::SharedPtr &client) {
        client->clear_on_new_response_callback();
      },
      [](const rclcpp::TimerBase::SharedPtr &timer) {
        UNUSED(timer);
      },
      [](const rclcpp::Waitable::SharedPtr &waitable) {
        waitable->clear_on_ready_callback();
      }
    );
  });
  this->remove_node(node_ptr->get_node_base_interface(), notify);
}

void
NoExecutor::handle_subscription(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::SubscriptionBase::SharedPtr &subscription, size_t num_msgs) {
  if (!started) {
    return;
  }
  while (num_msgs--) {
    Executable executable;
    executable.type = ExecutableType::SUBSCRIPTION;
    executable.callback_group = callback_group;
    executable.subscription = subscription;
    assign_or_create(executable);
  }
}

void
NoExecutor::handle_service(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::ServiceBase::SharedPtr &service, size_t num_msgs) {
  if (!started) {
    return;
  }
  while (num_msgs--) {
    Executable executable;
    executable.type = ExecutableType::SERVICE;
    executable.callback_group = callback_group;
    executable.service = service;
    assign_or_create(executable);
  }
}

void
NoExecutor::handle_client(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::ClientBase::SharedPtr &client, size_t num_msgs) {
  if (!started) {
    return;
  }
  while (num_msgs--) {
    Executable executable;
    executable.type = ExecutableType::CLIENT;
    executable.callback_group = callback_group;
    executable.client = client;
    assign_or_create(executable);
  }
}

void
NoExecutor::handle_waitable(rclcpp::CallbackGroup::SharedPtr callback_group, const rclcpp::Waitable::SharedPtr &waitable, size_t num_msgs) {
  if (!started) {
    return;
  }
  while (num_msgs--) {
    Executable executable;
    executable.type = ExecutableType::WAITABLE;
    executable.callback_group = callback_group;
    executable.waitable = waitable;
    assign_or_create(executable);
  }
}

void 
handle_timer(int sig, siginfo_t *si, void *uc) {
  UNUSED(sig);
  UNUSED(uc);
  PosixTimer *ptimer = static_cast<PosixTimer*>(si->_sifields._rt.si_sigval.sival_ptr);
  if (!ptimer->executor->started) {
    return;
  }
  if (ptimer->timer->is_canceled()) {
    timer_settime(ptimer->timerid, 0, &unset_timer, NULL);
    return;
  }
  Executable executable;
  executable.type = ExecutableType::TIMER;
  executable.callback_group = ptimer->callback_group;
  executable.timer = ptimer->timer;
  ptimer->executor->assign_or_create(executable);
}


std::shared_ptr<rclcpp::sched::SchedBase> get_sched_base(rclcpp::executors::Executable& executable) {
  switch (executable.type)
  {
  case ExecutableType::SUBSCRIPTION:
    return executable.subscription;
  case ExecutableType::SERVICE:
    return executable.service;
  case ExecutableType::CLIENT:
    return executable.client;
  case ExecutableType::WAITABLE:
    return executable.waitable;
  case ExecutableType::TIMER:
    return executable.timer;
  default:
    return nullptr;
  }
}

void
NoExecutor::assign_or_create(Executable& executable) {
  auto idle_thread = idle_threads.pop();
  if (idle_thread == nullptr) {
    create_thread(std::move(executable));
    return;
  }
  auto sched_base = get_sched_base(executable);
	assert(sched_base != nullptr);
	
  idle_thread->executable = std::move(executable);
  int res = 0;
  if (sched_base->sched_entity.edf_attr) {
    if (sched_base->sched_entity.is_source) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      //std::cout << "time in sec: " << now.tv_sec << std::endl;
      sched_base->sched_entity.edf_attr->abs_deadline = (uint64_t) now.tv_sec * SEC_IN_NSEC + now.tv_nsec + sched_base->sched_entity.relative_deadline;
    }
    //std::cout << "abs deadline is: " << sched_entity->edf_attr->abs_deadline << std::endl;
    res = (sched::update_deadline(idle_thread->pthread_id, sched_base->sched_entity.edf_attr) == false);
  } else {
    res = sched::syscall_sched_setattr(idle_thread->pid, &sched_base->sched_attr);
  }
	assert(res == 0);
  idle_thread->is_busy.set_val(1, true);
}

void 
NoExecutor::create_thread(Executable executable) {
  auto sched_base = get_sched_base(executable);
  if (sched_base->sched_entity.edf_attr) {
    if (sched_base->sched_entity.is_source) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      //std::cout << "time in sec: " << now.tv_sec << std::endl;
      sched_base->sched_entity.edf_attr->abs_deadline = (uint64_t) now.tv_sec * SEC_IN_NSEC + now.tv_nsec + sched_base->sched_entity.relative_deadline;
    }
    std::thread new_thread(std::bind(&NoExecutor::thread_start, this, std::move(executable)));
    sched::update_deadline(new_thread.native_handle(), sched_base->sched_entity.edf_attr);
    new_thread.detach();
  } else {
    std::thread new_thread(std::bind(&NoExecutor::thread_start, this, std::move(executable)));
    sched::syscall_sched_setattr(sched::get_pid(new_thread.native_handle()), &sched_base->sched_attr);
    new_thread.detach();
  }
}

void
NoExecutor::thread_start(Executable executable) {
  rclcpp::executors::ThreadDataNoExec thread_data;
  thread_data.executable = executable;
  thread_data.is_busy.set_val(1, false);
  pthread_t self = pthread_self();
  thread_data.pthread_id = self;
  thread_data.pid = sched::get_pid(self);
  while (true) {
    thread_data.is_busy.wait_on(0);
    this->execute_executable(thread_data.executable);
    thread_data.is_busy.set_val(0, false);
    this->idle_threads.push(&thread_data);
  }
}

void
NoExecutor::spin() {
  spinning.store(true);
  RCPPUTILS_SCOPE_EXIT(this->spinning.store(false); );

  {
    std::lock_guard<std::mutex> guard{mutex_};
    add_callback_groups_from_nodes_associated_to_executor();
  }
  apply_chain_priorities();

  start();
  while (rclcpp::ok(this->context_)) {
    sleep(600);
  }
  stop();
}

void
NoExecutor::apply_chain_priorities()
{
  if (!chain_priority_allocator_) {
    return;
  }

  std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> groups_by_name;
  std::unordered_map<std::string, std::shared_ptr<rclcpp::sched::SchedBase>> entities_by_name;

  const auto logger = rclcpp::get_logger("NoExecutor");
  auto register_named_entity =
    [&groups_by_name, &entities_by_name, logger](
    const std::string & name,
    const rclcpp::CallbackGroup::SharedPtr & callback_group,
    const std::shared_ptr<rclcpp::sched::SchedBase> & entity)
    {
      if (name.empty() || !callback_group || !entity) {
        return;
      }
      auto [entity_it, inserted] = entities_by_name.emplace(name, entity);
      if (!inserted) {
        RCLCPP_WARN(
          logger,
          "Duplicate callback name '%s'; keeping first registration",
          name.c_str());
        return;
      }
      groups_by_name.emplace(name, callback_group);
    };

  {
    std::lock_guard<std::mutex> guard{mutex_};
    for (const auto & pair : weak_groups_to_nodes_) {
      auto group = pair.first.lock();
      if (!group) {
        continue;
      }
      group->collect_all_ptrs(
        [&register_named_entity, &group](const rclcpp::SubscriptionBase::SharedPtr & subscription) {
          if (subscription) {
            register_named_entity(
              subscription->get_callback_name(),
              group,
              std::static_pointer_cast<rclcpp::sched::SchedBase>(subscription));
          }
        },
        [&register_named_entity, &group](const rclcpp::ServiceBase::SharedPtr & service) {
          if (service) {
            register_named_entity(
              service->get_callback_name(),
              group,
              std::static_pointer_cast<rclcpp::sched::SchedBase>(service));
          }
        },
        [&register_named_entity, &group](const rclcpp::ClientBase::SharedPtr & client) {
          if (client) {
            register_named_entity(
              client->get_callback_name(),
              group,
              std::static_pointer_cast<rclcpp::sched::SchedBase>(client));
          }
        },
        [&register_named_entity, &group](const rclcpp::TimerBase::SharedPtr & timer) {
          if (timer) {
            register_named_entity(
              timer->get_callback_name(),
              group,
              std::static_pointer_cast<rclcpp::sched::SchedBase>(timer));
          }
        },
        [&register_named_entity, &group](const rclcpp::Waitable::SharedPtr & waitable) {
          if (waitable) {
            register_named_entity(
              waitable->get_callback_name(),
              group,
              std::static_pointer_cast<rclcpp::sched::SchedBase>(waitable));
          }
        });
    }
  }

  if (groups_by_name.empty()) {
    RCLCPP_WARN(
      logger,
      "Chain priority allocator is set but no named callbacks are registered");
    return;
  }

  auto allocation = chain_priority_allocator_->allocate(groups_by_name);
  for (const auto & pair : allocation.callback_priorities) {
    const auto & callback_name = pair.first;
    const auto priority = pair.second;
    auto entity_it = entities_by_name.find(callback_name);
    if (entity_it == entities_by_name.end()) {
      RCLCPP_WARN(
        logger,
        "No callback entity registered for '%s'; skipping priority assignment",
        callback_name.c_str());
      continue;
    }
    auto & entity = entity_it->second;
    if (!entity) {
      RCLCPP_WARN(
        logger,
        "Callback entity for '%s' is no longer valid; skipping priority assignment",
        callback_name.c_str());
      continue;
    }

    entity->set_edf_attr(nullptr);
    rclcpp::sched::SchedAttr attr = entity->sched_attr;
    attr.sched_policy = SCHED_FIFO;
    attr.sched_priority = priority;
    attr.sched_flags = 0;
    attr.sched_runtime = 0;
    attr.sched_deadline = 0;
    attr.sched_period = 0;
    entity->set_sched_attr(attr);
  }
}
