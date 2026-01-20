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

#ifndef RCLCPP__CHAIN_YAML_PARSER_HPP_
#define RCLCPP__CHAIN_YAML_PARSER_HPP_

#include <yaml-cpp/yaml.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rclcpp/visibility_control.hpp"

namespace rclcpp
{

struct userChain
{
  std::string chain_name;
  std::vector<std::string> callbacks;
  std::uint32_t deadline;
  std::uint32_t period;
};

class RCLCPP_PUBLIC ChainYamlParser
{
public:
  ChainYamlParser() = default;
  ~ChainYamlParser() = default;

  void load_yaml_file(const std::string & yaml_file);
  bool parse();
  const std::unordered_map<std::string, userChain> & get_user_chains() const
  {
    return user_chains;
  }

private:
  static constexpr const char * CALLBACKS_KEY = "callbacks";
  static constexpr const char * DEADLINE_KEY = "deadline";
  static constexpr const char * PERIOD_KEY = "period";

  std::string yaml_file;
  YAML::Node yaml_node;
  std::unordered_map<std::string, userChain> user_chains;
};

}  // namespace rclcpp

#endif  // RCLCPP__CHAIN_YAML_PARSER_HPP_
