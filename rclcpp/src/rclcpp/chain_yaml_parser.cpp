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

#include "rclcpp/chain_yaml_parser.hpp"

#include <stdexcept>

#include "rclcpp/logging.hpp"

namespace rclcpp
{

void ChainYamlParser::load_yaml_file(const std::string & new_yaml_file)
{
  yaml_file = new_yaml_file;
  try {
    yaml_node = YAML::LoadFile(new_yaml_file);
  } catch (const YAML::Exception & e) {
    RCLCPP_ERROR(
      rclcpp::get_logger("ChainYamlParser"),
      "Failed to load YAML file: %s",
      e.what());
    throw std::runtime_error("Failed to load YAML file: " + std::string(e.what()));
  }
}

bool ChainYamlParser::parse()
{
  if (yaml_node.IsNull() || !yaml_node.IsMap()) {
    RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "YAML file is not a map");
    return false;
  }

  const auto & chains_node = yaml_node["chains"];
  if (!chains_node || !chains_node.IsMap()) {
    RCLCPP_ERROR(
      rclcpp::get_logger("ChainYamlParser"),
      "Missing or invalid 'chains' section in YAML file");
    return false;
  }

  for (const auto & chain_entry : chains_node) {
    const std::string chain_name = chain_entry.first.as<std::string>();
    const auto & chain_node = chain_entry.second;

    if (chain_name.empty()) {
      const auto msg = std::string("Chain with empty name in YAML file");
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }

    if (!chain_node.IsMap()) {
      const auto msg = "Chain '" + chain_name + "' is not a map";
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }

    const auto & callbacks_node = chain_node[CALLBACKS_KEY];
    if (!callbacks_node || !callbacks_node.IsSequence() || callbacks_node.size() == 0) {
      const auto msg =
        "Chain '" + chain_name + "' has missing or empty '" + CALLBACKS_KEY + "' field";
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }

    const auto & deadline_node = chain_node[DEADLINE_KEY];
    if (!deadline_node || !deadline_node.IsScalar()) {
      const auto msg =
        "Chain '" + chain_name + "' has missing or invalid '" + DEADLINE_KEY + "' field";
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }

    const auto & period_node = chain_node[PERIOD_KEY];
    if (!period_node || !period_node.IsScalar()) {
      const auto msg =
        "Chain '" + chain_name + "' has missing or invalid '" + PERIOD_KEY + "' field";
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }

    try {
      const auto callbacks_list = callbacks_node.as<std::vector<std::string>>();
      const auto deadline = deadline_node.as<std::uint32_t>();
      const auto period = period_node.as<std::uint32_t>();

      user_chains[chain_name] = userChain{chain_name, callbacks_list, deadline, period};
      RCLCPP_DEBUG(
        rclcpp::get_logger("ChainYamlParser"),
        "Successfully parsed chain '%s' with %zu callbacks",
        chain_name.c_str(),
        callbacks_list.size());
    } catch (const YAML::Exception & e) {
      const auto msg = "Failed to parse chain '" + chain_name + "': " + e.what();
      RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "%s", msg.c_str());
      throw std::runtime_error(msg);
    }
  }

  if (user_chains.empty()) {
    RCLCPP_ERROR(rclcpp::get_logger("ChainYamlParser"), "No valid chains found in YAML file");
    return false;
  }

  return true;
}

}  // namespace rclcpp
