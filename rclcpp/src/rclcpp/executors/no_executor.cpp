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
#include <cerrno>
#include <cstring>
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
NoExecutor::set_timer_period(const std::string & name, int64_t period_ns) {
  auto it = timer_period_config_.find(name);
  if (it != timer_period_config_.end()) {
    it->second.store(period_ns);
  } else {
    timer_period_config_[name].store(period_ns);
  }
}

int64_t
NoExecutor::get_timer_period(const std::string & name) const {
  auto it = timer_period_config_.find(name);
  if (it != timer_period_config_.end()) {
    return it->second.load();
  }
  return 0;  
}

void
NoExecutor::set_timer_period_config(const std::unordered_map<std::string, int64_t> & config) {
  for (const auto & pair : config) {
    timer_period_config_[pair.first].store(pair.second);
  }
}

void
NoExecutor::set_timer_hold_until(const std::string & name, int64_t hold_until_ns) {
  auto it = timer_hold_until_config_.find(name);
  if (it != timer_hold_until_config_.end()) {
    it->second.store(hold_until_ns);
  } else {
    timer_hold_until_config_[name].store(hold_until_ns);
  }
}

void
NoExecutor::set_timer_delay_next(const std::string & name, int64_t delay_ns) {
  auto it = timer_delay_next_config_.find(name);
  if (it != timer_delay_next_config_.end()) {
    it->second.store(delay_ns);
  } else {
    timer_delay_next_config_[name].store(delay_ns);
  }
}

void
NoExecutor::start() {
  auto logger = rclcpp::get_logger("NoExecutor");
  RCLCPP_INFO(logger, "Starting NoExecutor with %zu timers", timers.size());
  pid_t cur_tid = gettid();
  for (PosixTimer *timer: timers) {
    RCLCPP_INFO(logger, "Creating POSIX timer for period %lu ns", timer->period);
    timer_t timerId = 0;
    union sigval sigv;
    sigv.sival_ptr = (void *) timer;
    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGRTMAX;
    sev.sigev_value = sigv;
    sev._sigev_un._tid = cur_tid;
    /* specifies the action when receiving a signal */
    int create_res = timer_create(CLOCK_MONOTONIC, &sev, &timerId);
    if (create_res != 0) {
      RCLCPP_ERROR(logger, "Failed to create timer: %s", strerror(errno));
    }
    timer->timerid = timerId;
    RCLCPP_INFO(logger, "Created timer ID: %p", (void*)timerId);
  }

  struct sigaction sa = {};
  sa.sa_flags = (SA_SIGINFO | SA_NODEFER | SA_RESTART);
  sa.sa_sigaction = handle_timer;
  sigemptyset(&sa.sa_mask);
  int sigaction_res = sigaction(SIGRTMAX, &sa, NULL);
  if (sigaction_res != 0) {
    RCLCPP_ERROR(logger, "Failed to install signal handler: %s", strerror(errno));
  } else {
    RCLCPP_INFO(logger, "Installed signal handler for SIGRTMAX (%d)", SIGRTMAX);
  }
  
  started = true;
  RCLCPP_INFO(logger, "Arming timers, starting execution...");
  for (PosixTimer *timer: timers) {
    /* specify start delay and interval */
    struct itimerspec its = {};
    struct timespec it_interval = {};
    struct timespec it_value = {};
    it_interval.tv_nsec = timer->period % SEC_IN_NSEC;
    it_interval.tv_sec = timer->period / SEC_IN_NSEC;
    // Start with first expiration at 1ms to give time for setup
    it_value.tv_nsec = 1000000;  // 1ms initial delay
    it_value.tv_sec = 0;
    its.it_interval = it_interval;
    its.it_value = it_value;
    RCLCPP_INFO(logger, "Arming timer %p: interval=%ld.%09ld, value=%ld.%09ld", 
      (void*)timer->timerid,
      (long)it_interval.tv_sec, (long)it_interval.tv_nsec,
      (long)it_value.tv_sec, (long)it_value.tv_nsec);
    int res = timer_settime(timer->timerid, 0, &its, NULL);
    if (res != 0) {
      RCLCPP_ERROR(logger, "Failed to arm timer %p (errno=%d): %s", 
        (void*)timer->timerid, errno, strerror(errno));
    } else {
      RCLCPP_INFO(logger, "Successfully armed timer %p", (void*)timer->timerid);
    }
  }
  RCLCPP_INFO(logger, "All timers armed, entering spin loop");
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
    executable.subscription->run();
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
        //using cb name as key in map
        std::string timer_name = timer->get_callback_name();
        
        int64_t period = 0;
        auto config_it = timer_period_config_.find(timer_name);
        if (config_it != timer_period_config_.end()) {
          period = config_it->second.load();
        } else {
          period = get_period_from_timer(timer);
          timer_period_config_[timer_name].store(period);
        }
        
        std::atomic<int64_t>* period_ptr = &timer_period_config_[timer_name];
        timer_period_map_[timer.get()] = period_ptr;

        timer_hold_until_config_[timer_name].store(0);
        timer_delay_next_config_[timer_name].store(0);
        std::atomic<int64_t>* hold_until_ptr = &timer_hold_until_config_[timer_name];
        std::atomic<int64_t>* delay_next_ptr = &timer_delay_next_config_[timer_name];

        this->timers.push_back(new PosixTimer({
          this, static_cast<uint64_t>(period), period_ptr,
          hold_until_ptr, delay_next_ptr,
          timer, callback_group, 0, this->timers.size()
        }));
      },
      [this, &callback_group](const rclcpp::Waitable::SharedPtr &waitable) {
        waitable->set_on_ready_callback(std::bind(&NoExecutor::handle_waitable, this, callback_group, waitable, _1));
      }
    );
  });

  // Don't apply chain priorities here - wait until all nodes are added
  // apply_chain_priorities() will be called in spin()
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
    executable.subscription = rclcpp::take_and_bundle(subscription);
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
  if (ptimer == nullptr) {
    return;
  }
  if (ptimer->executor == nullptr || !ptimer->executor->started) {
    return;
  }
  if (ptimer->timer == nullptr) {
    return;
  }
  if (ptimer->timer->is_canceled()) {
    timer_settime(ptimer->timerid, 0, &unset_timer, NULL);
    return;
  }
  
  if (ptimer->period_ptr != nullptr) {
    int64_t new_period = ptimer->period_ptr->load();
    if (new_period != static_cast<int64_t>(ptimer->period) && new_period > 0) {
      struct itimerspec its = {};
      its.it_interval.tv_nsec = new_period % SEC_IN_NSEC;
      its.it_interval.tv_sec = new_period / SEC_IN_NSEC;
      its.it_value.tv_nsec = new_period % SEC_IN_NSEC;
      its.it_value.tv_sec = new_period / SEC_IN_NSEC;
      
      int res = timer_settime(ptimer->timerid, 0, &its, NULL);
      if (res == 0) {
        ptimer->period = static_cast<uint64_t>(new_period);
      }
    }
  }

  // B/C: Timer held until hold_until time; auto-clear when time is reached
  if (ptimer->hold_until_ptr != nullptr) {
    int64_t hold_until = ptimer->hold_until_ptr->load();
    if (hold_until > 0) {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      int64_t now_ns = (int64_t)now.tv_sec * SEC_IN_NSEC + now.tv_nsec;

      if (now_ns < hold_until) {
        // Not yet time — reschedule with hold_until as next fire, then resume periodic
        struct itimerspec its = {};
        its.it_value.tv_sec  = hold_until / SEC_IN_NSEC;
        its.it_value.tv_nsec = hold_until % SEC_IN_NSEC;
        its.it_interval.tv_nsec = ptimer->period % SEC_IN_NSEC;
        its.it_interval.tv_sec  = ptimer->period / SEC_IN_NSEC;
        timer_settime(ptimer->timerid, TIMER_ABSTIME, &its, NULL);
        return;
      }

      // Time reached — clear hold, fall through to execute the callback
      ptimer->hold_until_ptr->store(0);
    }
  }

  // D: Delay next invocation by a relative offset
  if (ptimer->delay_next_ptr != nullptr) {
    int64_t delay = ptimer->delay_next_ptr->exchange(0);
    if (delay > 0) {
      struct itimerspec its = {};
      its.it_value.tv_nsec  = delay % SEC_IN_NSEC;
      its.it_value.tv_sec   = delay / SEC_IN_NSEC;
      its.it_interval.tv_nsec = ptimer->period % SEC_IN_NSEC;
      its.it_interval.tv_sec  = ptimer->period / SEC_IN_NSEC;
      timer_settime(ptimer->timerid, 0, &its, NULL);
      return;  // don't execute this time
    }
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
    return executable.subscription->get();
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

uint32_t get_mode_prio(rclcpp::executors::Executable& executable) {
  switch (executable.type)
  {
  case ExecutableType::SUBSCRIPTION:
    return executable.subscription->get_message_prio();
  case ExecutableType::TIMER:
    return executable.timer->sched_attr.sched_priority; // TODO: mode-based priority for timers
  default:
    return 0;
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
  if (sched_base == nullptr) {
    auto logger = rclcpp::get_logger("NoExecutor");
    RCLCPP_ERROR(logger, "sched_base is nullptr in assign_or_create");
    return;
  }
	
  idle_thread->executable = std::move(executable);
  int res = 0;
  uint32_t mode_prio = get_mode_prio(idle_thread->executable);
  if (0 < mode_prio && mode_prio < 100) {
    auto mode_sched_attr = sched_base->sched_attr;
    mode_sched_attr.sched_priority = mode_prio;
    res = sched::syscall_sched_setattr(idle_thread->pid, &mode_sched_attr);
  } else {
    res = sched::syscall_sched_setattr(idle_thread->pid, &sched_base->sched_attr);
  }
  if (res != 0) {
    // Only warn once about permission issues
    static std::atomic<bool> warned_once{false};
    if (!warned_once.exchange(true)) {
      auto logger = rclcpp::get_logger("NoExecutor");
      RCLCPP_WARN(logger, 
        "Failed to set real-time scheduling attributes (SCHED_FIFO). "
        "This requires elevated privileges (CAP_SYS_NICE or running as root). "
        "Node will continue with default scheduling. "
        "To enable RT scheduling, run with 'sudo' or configure /etc/security/limits.conf");
    }
  }
  idle_thread->is_busy.set_val(1, true);
}

void 
NoExecutor::create_thread(Executable executable) {
  auto sched_base = get_sched_base(executable);
  if (sched_base == nullptr) {
    auto logger = rclcpp::get_logger("NoExecutor");
    RCLCPP_ERROR(logger, "sched_base is nullptr in create_thread");
    return;
  }
  std::thread new_thread(std::bind(&NoExecutor::thread_start, this, std::move(executable)));
  sched::syscall_sched_setattr(sched::get_pid(new_thread.native_handle()), &sched_base->sched_attr);
  new_thread.detach();
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

NoExecutor::NamedEntities
NoExecutor::collect_named_entities()
{
  NamedEntities named;
  const auto logger = rclcpp::get_logger("NoExecutor");

  auto register_named_entity =
    [&named, logger](
    const std::string & name,
    const rclcpp::CallbackGroup::SharedPtr & callback_group,
    const std::shared_ptr<rclcpp::sched::SchedBase> & entity)
    {
      if (name.empty() || !callback_group || !entity) {
        if (name.empty() && entity) {
          RCLCPP_DEBUG(logger, "Skipping entity with empty callback name");
        }
        return;
      }
      RCLCPP_INFO(logger, "Registering callback: '%s'", name.c_str());
      auto [entity_it, inserted] = named.entities_by_name.emplace(name, entity);
      if (!inserted) {
        RCLCPP_WARN(
          logger,
          "Duplicate callback name '%s'; keeping first registration",
          name.c_str());
        return;
      }
      named.groups_by_name.emplace(name, callback_group);
    };

  {
    std::lock_guard<std::mutex> guard{mutex_};
    for (const auto & pair : weak_groups_to_nodes_) {
      auto group = pair.first.lock();
      if (!group) {
        continue;
      }
      group->collect_all_ptrs(
        [&register_named_entity, &group, &logger](const rclcpp::SubscriptionBase::SharedPtr & subscription) {
          if (subscription) {
            auto callback_name = subscription->get_callback_name();
            RCLCPP_DEBUG(logger, "Found subscription with callback name: '%s'", callback_name.c_str());
            register_named_entity(
              callback_name,
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
        [&register_named_entity, &group, &logger](const rclcpp::TimerBase::SharedPtr & timer) {
          if (timer) {
            auto callback_name = timer->get_callback_name();
            RCLCPP_DEBUG(logger, "Found timer with callback name: '%s'", callback_name.c_str());
            register_named_entity(
              callback_name,
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

  RCLCPP_INFO(logger, "Collected %zu named callbacks", named.groups_by_name.size());
  for (const auto & pair : named.groups_by_name) {
    RCLCPP_INFO(logger, "  - '%s'", pair.first.c_str());
  }

  return named;
}

void
NoExecutor::apply_sched_attr_to_entity(
  const std::shared_ptr<rclcpp::sched::SchedBase> & entity,
  uint32_t new_policy,
  uint32_t new_priority)
{
  auto attr = entity->sched_attr;
  attr.sched_policy = new_policy;
  attr.sched_priority = new_priority;
  entity->set_sched_attr(attr);
}

void
NoExecutor::apply_chain_priorities()
{
  if (!chain_priority_allocator_) {
    return;
  }

  auto named = collect_named_entities();

  const auto logger = rclcpp::get_logger("NoExecutor");

  if (named.groups_by_name.empty()) {
    RCLCPP_WARN(
      logger,
      "Chain priority allocator is set but no named callbacks are registered");
    return;
  }

  auto allocation = chain_priority_allocator_->allocate(named.groups_by_name);

  RCLCPP_INFO(logger, "Chain priority allocation results:");
  for (const auto & pair : allocation.callback_priorities) {
    RCLCPP_INFO(logger, "  '%s' -> priority %d", pair.first.c_str(), pair.second);
  }

  for (const auto & pair : allocation.callback_priorities) {
    const auto & callback_name = pair.first;
    const auto priority = pair.second;
    auto entity_it = named.entities_by_name.find(callback_name);
    if (entity_it == named.entities_by_name.end()) {
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

    apply_sched_attr_to_entity(entity, SCHED_FIFO, priority);
  }
  RCLCPP_INFO(logger, "Priority allocation complete");
}
