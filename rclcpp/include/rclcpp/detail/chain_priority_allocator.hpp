// Copyright 2025 Open Source Robotics Foundation, Inc.
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

#ifndef RCLCPP__DETAIL__CHAIN_PRIORITY_ALLOCATOR_HPP_
#define RCLCPP__DETAIL__CHAIN_PRIORITY_ALLOCATOR_HPP_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rclcpp/callback_group.hpp"
#include "rclcpp/chain_yaml_parser.hpp"
#include "rclcpp/visibility_control.hpp"

namespace rclcpp
{
namespace detail
{

struct ThreadGroupInfo
{
  int threadgroup_id = 0;
  std::vector<std::string> callbacks;
  std::uint16_t fixed_priority = 0;
  bool is_mutex_group = false;
};

struct ChainPriorityAllocation
{
  std::unordered_map<std::string, std::uint16_t> callback_priorities;
  std::unordered_map<std::string, std::uint32_t> callback_periods;  // name -> chain period
  std::unordered_map<int, ThreadGroupInfo> threadgroups;
};

class RCLCPP_PUBLIC ChainPriorityAllocator
{
public:
  explicit ChainPriorityAllocator(
    std::shared_ptr<const std::unordered_map<std::string, userChain>> user_chains);

  ChainPriorityAllocation allocate(
    const std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> &
    callback_groups_by_name);

private:
  struct CallbackAdjacencyInfo
  {
    std::unordered_set<std::string> outgoing = {};
    std::uint8_t indegree = 0;
    std::vector<std::uint32_t> deadlines = {};
    std::vector<std::uint32_t> periods = {};
    std::uint32_t min_deadline = UINT32_MAX;
    std::uint32_t min_deadline_period = 0;  // period of the chain with min_deadline
  };

  struct ThreadGroupAdjacencyInfo
  {
    std::unordered_set<int> outgoing = {};
    std::unordered_set<int> incoming = {};

    std::uint8_t indegree() const {return static_cast<std::uint8_t>(incoming.size());}
  };

  struct CallbackInfo
  {
    std::string callback_name;
    rclcpp::CallbackGroup::SharedPtr callback_group;
    int threadgroup_id = 0;
  };

  void reset_state();
  void build_adjacency_list(
    const std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> &
    callback_groups_by_name);
  void recursive_callback_traversal(
    const std::string & callback_name,
    int threadgroup_id,
    int prev_threadgroup_id,
    std::map<std::uint32_t, std::vector<int>> & deadline_to_threadgroup_id_map);
  int generate_threadgroup_id();

  std::shared_ptr<const std::unordered_map<std::string, userChain>> user_chains_;

  std::unordered_map<std::string, CallbackAdjacencyInfo> adjacency_list_;
  std::unordered_map<int, ThreadGroupAdjacencyInfo> threadgroup_adjacency_list_;
  std::unordered_map<rclcpp::CallbackGroup::SharedPtr, int> mutex_threadgroup_map_;
  std::unordered_map<std::string, CallbackInfo> callback_map_;
  std::unordered_map<int, ThreadGroupInfo> threadgroup_callback_map_;

  int next_threadgroup_id_ = 1;
};

}  // namespace detail
}  // namespace rclcpp

#endif  // RCLCPP__DETAIL__CHAIN_PRIORITY_ALLOCATOR_HPP_
