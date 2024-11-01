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

#ifndef RCLCPP__ANY_EXECUTABLE_HPP_
#define RCLCPP__ANY_EXECUTABLE_HPP_

#include <memory>

#include "rclcpp/callback_group.hpp"
#include "rclcpp/client.hpp"
#include "rclcpp/macros.hpp"
#include "rclcpp/node_interfaces/node_base_interface.hpp"
#include "rclcpp/service.hpp"
#include "rclcpp/subscription.hpp"
#include "rclcpp/timer.hpp"
#include "rclcpp/visibility_control.hpp"
#include "rclcpp/waitable.hpp"

namespace rclcpp
{

struct AnyExecutable
{
  RCLCPP_PUBLIC
  AnyExecutable();
	AnyExecutable(const AnyExecutable& anyexec) = default;
	AnyExecutable(AnyExecutable&& anyexec) = default;
	AnyExecutable& operator=(AnyExecutable&& anyexec) = default;


  RCLCPP_PUBLIC
  virtual ~AnyExecutable();

  RCLCPP_PUBLIC
  bool operator==(const AnyExecutable& other) const {
    return (subscription == other.subscription) &&
           (timer == other.timer) &&
           (service == other.service) &&
           (client == other.client) &&
           (waitable == other.waitable) &&
           (callback_group == other.callback_group) &&
           (node_base == other.node_base) &&
           (data == other.data);
  }


  // Only one of the following pointers will be set.
  rclcpp::SubscriptionBase::SharedPtr subscription;
  rclcpp::TimerBase::SharedPtr timer;
  rclcpp::ServiceBase::SharedPtr service;
  rclcpp::ClientBase::SharedPtr client;
  rclcpp::Waitable::SharedPtr waitable;
  // These are used to keep the scope on the containing items
  rclcpp::CallbackGroup::SharedPtr callback_group;
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr node_base;
  std::shared_ptr<void> data;
};

struct AnyExecutableHash {
    RCLCPP_PUBLIC
    std::size_t operator()(const AnyExecutable& anyExec) const {
        std::size_t hash = 0;
        // Combine hashes of the pointers; use std::hash to hash individual pointers
        hash ^= std::hash<decltype(anyExec.subscription)>()(anyExec.subscription);
        hash ^= std::hash<decltype(anyExec.timer)>()(anyExec.timer);
        hash ^= std::hash<decltype(anyExec.service)>()(anyExec.service);
        hash ^= std::hash<decltype(anyExec.client)>()(anyExec.client);
        hash ^= std::hash<decltype(anyExec.waitable)>()(anyExec.waitable);
        hash ^= std::hash<decltype(anyExec.callback_group)>()(anyExec.callback_group);
        hash ^= std::hash<decltype(anyExec.node_base)>()(anyExec.node_base);
        hash ^= std::hash<void*>()(anyExec.data.get()); // Hash the raw pointer from the shared_ptr
        return hash;
    }
};


}  // namespace rclcpp

#endif  // RCLCPP__ANY_EXECUTABLE_HPP_
