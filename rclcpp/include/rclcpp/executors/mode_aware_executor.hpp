#ifndef RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_
#define RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "rclcpp/chain_yaml_parser.hpp"
#include "rclcpp/detail/chain_priority_allocator.hpp"
#include "rclcpp/executors/no_executor.hpp"
#include "rclcpp/logging.hpp"

namespace rclcpp
{
namespace executors
{

/// An executor that supports runtime switching between different chain-priority modes.
/**
 * Each mode is defined by a YAML file specifying callback chains with deadlines/periods.
 * A user-provided mode tester function maps application state to the desired mode.
 * Calling mode_update() checks the state and, if the mode has changed, re-applies
 * the corresponding chain priorities.
 *
 * \tparam StateT  The application state type passed to the mode tester function.
 * \tparam ModeEnumT  An enum (or enum class) identifying each mode.
 */
template<typename StateT, typename ModeEnumT>
class ModeAwareExecutor : public NoExecutor
{
public:
  using ModeTesterFn = std::function<ModeEnumT(const StateT &)>;

  /// Construct a mode-aware executor.
  /**
   * \param[in] mode_yaml_map  Mapping from mode enum values to YAML file paths.
   * \param[in] mode_tester    Function that maps application state to the desired mode.
   * \param[in] initial_mode   The mode to activate at startup.
   * \param[in] options        Executor options forwarded to the base class.
   * \throws std::invalid_argument if mode_yaml_map is empty, mode_tester is null,
   *         or initial_mode is not found in mode_yaml_map.
   */
  explicit ModeAwareExecutor(
    std::map<ModeEnumT, std::string> mode_yaml_map,
    ModeTesterFn mode_tester,
    ModeEnumT initial_mode,
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions())
  : NoExecutor(options, nullptr),
    mode_tester_(std::move(mode_tester)),
    current_mode_(initial_mode)
  {
    if (mode_yaml_map.empty()) {
      throw std::invalid_argument("ModeAwareExecutor: mode_yaml_map must not be empty");
    }
    if (!mode_tester_) {
      throw std::invalid_argument("ModeAwareExecutor: mode_tester must be callable");
    }
    if (mode_yaml_map.find(initial_mode) == mode_yaml_map.end()) {
      throw std::invalid_argument("ModeAwareExecutor: initial_mode not found in mode_yaml_map");
    }

    const auto logger = rclcpp::get_logger("ModeAwareExecutor");

    for (const auto & [mode, yaml_path] : mode_yaml_map) {
      RCLCPP_INFO(logger, "Loading mode YAML: %s", yaml_path.c_str());

      rclcpp::ChainYamlParser parser;
      parser.load_yaml_file(yaml_path);
      if (!parser.parse()) {
        throw std::runtime_error(
                "ModeAwareExecutor: failed to parse YAML file: " + yaml_path);
      }

      // Store an owned copy of the parsed chains (allocator holds a const reference)
      mode_chains_[mode] = parser.get_user_chains();

      mode_allocators_[mode] = std::make_shared<rclcpp::detail::ChainPriorityAllocator>(
        mode_chains_[mode]);
    }

    // Activate the initial mode
    this->chain_priority_allocator_ = mode_allocators_[initial_mode];
  }

  /// Check the current state and switch modes if the mode tester returns a different mode.
  /**
   * If the mode has changed, the new mode's chain priority allocator is installed
   * and apply_chain_priorities() is called to re-apply SCHED_FIFO priorities.
   *
   * This should be called at well-defined quiescent points (e.g., from a timer callback).
   *
   * \param[in] state  The current application state to evaluate.
   */
  void mode_update(const StateT & state)
  {
    ModeEnumT target_mode = mode_tester_(state);

    if (target_mode == current_mode_) {
      return;
    }

    auto it = mode_allocators_.find(target_mode);
    if (it == mode_allocators_.end()) {
      const auto logger = rclcpp::get_logger("ModeAwareExecutor");
      RCLCPP_ERROR(
        logger,
        "mode_update: target mode not found in mode_allocators_; ignoring switch");
      return;
    }

    const auto logger = rclcpp::get_logger("ModeAwareExecutor");
    RCLCPP_INFO(logger, "Switching mode: %d -> %d",
      static_cast<int>(current_mode_), static_cast<int>(target_mode));

    current_mode_ = target_mode;
    this->chain_priority_allocator_ = it->second;
    this->apply_chain_priorities();
  }

  /// Return the currently active mode.
  ModeEnumT get_current_mode() const
  {
    return current_mode_;
  }

private:
  // mode_chains_ must be declared before mode_allocators_ so that chains
  // (referenced by allocators) outlive the allocators during destruction.
  std::map<ModeEnumT, std::unordered_map<std::string, rclcpp::userChain>> mode_chains_;
  std::map<ModeEnumT, std::shared_ptr<rclcpp::detail::ChainPriorityAllocator>> mode_allocators_;

  ModeTesterFn mode_tester_;
  ModeEnumT current_mode_;
};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_
