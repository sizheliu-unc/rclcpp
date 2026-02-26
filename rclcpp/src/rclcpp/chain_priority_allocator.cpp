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

#include "rclcpp/detail/chain_priority_allocator.hpp"

#include "rclcpp/logging.hpp"

namespace rclcpp
{
namespace detail
{
namespace
{
rclcpp::Logger get_chain_priority_logger()
{
  return rclcpp::get_logger("ChainPriorityAllocator");
}
}  // namespace

ChainPriorityAllocator::ChainPriorityAllocator(
  std::shared_ptr<const std::unordered_map<std::string, userChain>> user_chains)
: user_chains_(std::move(user_chains))
{
}

ChainPriorityAllocation ChainPriorityAllocator::allocate(
  const std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> &
  callback_groups_by_name)
{
  reset_state();
  build_adjacency_list(callback_groups_by_name);

  std::map<std::uint32_t, std::vector<int>> deadline_to_threadgroup_id_map;
  for (const auto & pair : adjacency_list_) {
    if (pair.second.indegree == 0) {
      recursive_callback_traversal(pair.first, 0, 0, deadline_to_threadgroup_id_map);
    }
  }

  std::uint16_t fixed_priority_counter = 1;
  for (const auto & pair : deadline_to_threadgroup_id_map) {
    for (const auto & threadgroup_id : pair.second) {
      auto & threadgroup_info = threadgroup_callback_map_[threadgroup_id];
      if (threadgroup_info.fixed_priority == 0) {
        threadgroup_info.fixed_priority = fixed_priority_counter++;
      }
    }
  }

  ChainPriorityAllocation allocation;
  allocation.threadgroups = threadgroup_callback_map_;

  const auto logger = get_chain_priority_logger();
  for (const auto & pair : callback_map_) {
    const auto & callback_info = pair.second;
    if (callback_info.threadgroup_id == 0) {
      RCLCPP_WARN(
        logger,
        "Callback '%s' was not assigned a threadgroup; skipping priority assignment",
        pair.first.c_str());
      continue;
    }
    const auto threadgroup_it = threadgroup_callback_map_.find(callback_info.threadgroup_id);
    if (threadgroup_it == threadgroup_callback_map_.end() ||
      threadgroup_it->second.fixed_priority == 0)
    {
      RCLCPP_WARN(
        logger,
        "Callback '%s' threadgroup '%d' missing priority; skipping priority assignment",
        pair.first.c_str(),
        callback_info.threadgroup_id);
      continue;
    }
    allocation.callback_priorities.emplace(pair.first, threadgroup_it->second.fixed_priority);

    const auto adj_it = adjacency_list_.find(pair.first);
    if (adj_it != adjacency_list_.end()) {
      allocation.callback_periods.emplace(pair.first, adj_it->second.min_deadline_period);
    }
  }

  return allocation;
}

void ChainPriorityAllocator::reset_state()
{
  adjacency_list_.clear();
  threadgroup_adjacency_list_.clear();
  mutex_threadgroup_map_.clear();
  callback_map_.clear();
  threadgroup_callback_map_.clear();
  next_threadgroup_id_ = 1;
}

void ChainPriorityAllocator::build_adjacency_list(
  const std::unordered_map<std::string, rclcpp::CallbackGroup::SharedPtr> &
  callback_groups_by_name)
{
  const auto logger = get_chain_priority_logger();
  for (const auto & pair : callback_groups_by_name) {
    if (!pair.second) {
      RCLCPP_WARN(
        logger,
        "Callback '%s' has a null callback group; skipping",
        pair.first.c_str());
      continue;
    }
    callback_map_.emplace(
      pair.first,
      CallbackInfo{pair.first, pair.second, 0});
  }

  for (const auto & chain_pair : *user_chains_) {
    const auto & chain_name = chain_pair.first;
    const auto & chain = chain_pair.second;
    const auto & callbacks = chain.callbacks;

    std::string prev_present;
    bool has_prev = false;
    for (const auto & callback_name : callbacks) {
      if (callback_map_.find(callback_name) == callback_map_.end()) {
        RCLCPP_WARN(
          logger,
          "Chain '%s' references unknown callback '%s'",
          chain_name.c_str(),
          callback_name.c_str());
        has_prev = false;
        continue;
      }

      auto & adj_info = adjacency_list_[callback_name];
      adj_info.deadlines.push_back(chain.deadline);
      adj_info.periods.push_back(chain.period);
      if (chain.deadline < adj_info.min_deadline) {
        adj_info.min_deadline = chain.deadline;
        adj_info.min_deadline_period = chain.period;
      }

      if (has_prev) {
        adjacency_list_[prev_present].outgoing.emplace(callback_name);
        adjacency_list_[callback_name].indegree++;
      }
      prev_present = callback_name;
      has_prev = true;
    }
  }
}

void ChainPriorityAllocator::recursive_callback_traversal(
  const std::string & callback_name,
  int threadgroup_id,
  int prev_threadgroup_id,
  std::map<std::uint32_t, std::vector<int>> & deadline_to_threadgroup_id_map)
{
  const auto logger = get_chain_priority_logger();
  const auto callback_it = callback_map_.find(callback_name);
  if (callback_it == callback_map_.end()) {
    RCLCPP_WARN(
      logger,
      "Callback '%s' not registered; skipping traversal",
      callback_name.c_str());
    return;
  }
  auto & callback_info = callback_it->second;
  if (callback_info.threadgroup_id != 0) {
    return;
  }

  if (threadgroup_id) {
    callback_info.threadgroup_id = threadgroup_id;
  }

  const bool is_mutex_group =
    callback_info.callback_group &&
    callback_info.callback_group->type() == rclcpp::CallbackGroupType::MutuallyExclusive;

  if (threadgroup_id == 0 || is_mutex_group) {
    int new_threadgroup_id;
    if (is_mutex_group) {
      auto existing_threadgroup_it = mutex_threadgroup_map_.find(callback_info.callback_group);
      if (existing_threadgroup_it != mutex_threadgroup_map_.end()) {
        new_threadgroup_id = existing_threadgroup_it->second;
      } else {
        new_threadgroup_id = generate_threadgroup_id();
        mutex_threadgroup_map_[callback_info.callback_group] = new_threadgroup_id;
        threadgroup_callback_map_[new_threadgroup_id].is_mutex_group = true;
      }
    } else {
      new_threadgroup_id = generate_threadgroup_id();
    }

    threadgroup_callback_map_[new_threadgroup_id].callbacks.push_back(callback_name);
    threadgroup_callback_map_[new_threadgroup_id].threadgroup_id = new_threadgroup_id;
    callback_info.threadgroup_id = new_threadgroup_id;

    const auto adj_it = adjacency_list_.find(callback_name);
    if (adj_it != adjacency_list_.end()) {
      deadline_to_threadgroup_id_map[adj_it->second.min_deadline].push_back(new_threadgroup_id);
    }

    if (threadgroup_id == 0) {
      threadgroup_id = new_threadgroup_id;
      if (prev_threadgroup_id != 0) {
        threadgroup_adjacency_list_[prev_threadgroup_id].outgoing.insert(new_threadgroup_id);
        threadgroup_adjacency_list_[new_threadgroup_id].incoming.insert(prev_threadgroup_id);
      }
    }
  }

  const auto adj_it = adjacency_list_.find(callback_name);
  if (adj_it == adjacency_list_.end()) {
    return;
  }
  const auto & outgoing = adj_it->second.outgoing;
  if (outgoing.empty()) {
    return;
  }

  const auto & next_callback = *outgoing.begin();
  if (outgoing.size() == 1) {
    threadgroup_callback_map_[threadgroup_id].callbacks.push_back(next_callback);
  }

  const auto next_adj_it = adjacency_list_.find(next_callback);
  if (next_adj_it != adjacency_list_.end() && next_adj_it->second.indegree == 1) {
    return recursive_callback_traversal(
      next_callback,
      threadgroup_id,
      prev_threadgroup_id,
      deadline_to_threadgroup_id_map);
  }

  for (const auto & next_callback_name : outgoing) {
    if (!threadgroup_callback_map_[threadgroup_id].is_mutex_group) {
      prev_threadgroup_id = threadgroup_id;
    }
    recursive_callback_traversal(
      next_callback_name,
      0,
      threadgroup_id,
      deadline_to_threadgroup_id_map);
  }
}

int ChainPriorityAllocator::generate_threadgroup_id()
{
  return next_threadgroup_id_++;
}

}  // namespace detail
}  // namespace rclcpp
