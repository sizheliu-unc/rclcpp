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

#include "rclcpp/executors/no_executor.hpp"
#include "rclcpp/sched_base.hpp"

#include "tracetools/tracetools.h"

#define SEC_IN_NSEC 1'000'000'000

using std::placeholders::_1;
using rclcpp::executors::NoExecutor;
using rclcpp::executors::Executable;
using rclcpp::executors::ExecutableType;


NoExecutor::NoExecutor(const rclcpp::ExecutorOptions & options)
: rclcpp::Executor(options) {
  started = false;
}

NoExecutor::~NoExecutor() {}

void
NoExecutor::start() {
  started = true;
}

void
NoExecutor::stop() {
  started = false;
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
  default:
    break;
  }
  if (executable.callback_group->type() == CallbackGroupType::MutuallyExclusive) {
    executable.callback_group->callback_group_mutex.unlock();
    executable.callback_group.reset();
  }
}

void
NoExecutor::add_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify) {
  this->add_node(node_ptr->get_node_base_interface(), notify);
  node_ptr->for_each_callback_group([this](rclcpp::CallbackGroup::SharedPtr callback_group) {
    callback_group->collect_all_ptrs(
      [this, &callback_group](const rclcpp::SubscriptionBase::SharedPtr &subscription) {
        subscription->set_on_new_message_callback(std::bind(&NoExecutor::handle_subscription, this, callback_group, subscription, _1));
      },
      [this, &callback_group](const rclcpp::ServiceBase::SharedPtr &service) {
        service->set_on_new_request_callback(std::bind(&NoExecutor::handle_service, this, callback_group, service, _1));
      },
      [this, &callback_group](const rclcpp::ClientBase::SharedPtr &client) {
        client->set_on_new_response_callback(std::bind(&NoExecutor::handle_client, this, callback_group, client, _1));
      },
      [](const rclcpp::TimerBase::SharedPtr &timer) {
          
      },
      [this, &callback_group](const rclcpp::Waitable::SharedPtr &waitable) {
        waitable->set_on_ready_callback(std::bind(&NoExecutor::handle_waitable, this, callback_group, waitable, _1));
      }
    );
  });
}

void
NoExecutor::remove_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify)
{
  node_ptr->for_each_callback_group([&](rclcpp::CallbackGroup::SharedPtr callback_group) {
    callback_group->collect_all_ptrs(
      [](const rclcpp::SubscriptionBase::SharedPtr &subscription) {
        subscription->clear_on_new_message_callback();
      },
      [](const rclcpp::ServiceBase::SharedPtr &service) {
        service->clear_on_new_request_callback();
      },
      [](const rclcpp::ClientBase::SharedPtr &client) {
        client->clear_on_new_response_callback();
      },
      [](const rclcpp::TimerBase::SharedPtr &timer) {
          
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
NoExecutor::spin() {}
