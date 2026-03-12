#ifndef RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_
#define RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_

#include <atomic>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sched.h>

#include "rclcpp/chain_yaml_parser.hpp"
#include "rclcpp/detail/chain_priority_allocator.hpp"
#include "rclcpp/executors/no_executor.hpp"
#include "rclcpp/logging.hpp"

namespace rclcpp
{
namespace executors
{

/// An executor that supports runtime mode switching with offset-based transitions.
/**
 * Implements the mode change protocol from Section 6.2 of Real & Crespo (2004).
 * Each mode is defined by a YAML file specifying callback chains with deadlines/periods.
 * When a mode change request (MCR) occurs, callbacks are classified as:
 *   - Old-mode completed/aborted: demoted to SCHED_OTHER immediately
 *   - Changed/wholly new: priority+period applied immediately, timer held until MCR+Y_i
 *   - Unchanged: continue uninterrupted (or with Z_i delay if provided)
 *
 * Hold clearing is done by handle_timer auto-clear when time is reached.
 *
 * \tparam StateT   The application state type passed to the mode tester function.
 * \tparam ModeEnumT  An enum (or enum class) identifying each mode.
 */
template<typename StateT, typename ModeEnumT>
class ModeAwareExecutor : public NoExecutor
{
public:
  using ModeTesterFn = std::function<ModeEnumT(const StateT &)>;

  /// Maps (from_mode, to_mode) -> (callback_name -> offset_nanoseconds).
  /// Y offsets for changed/wholly-new tasks, Z offsets for unchanged tasks.
  /// All offsets are relative to t_MCR.
  using ModeOffsetMap = std::map<
    std::pair<ModeEnumT, ModeEnumT>,
    std::map<std::string, int64_t>>;

  /// Construct a mode-aware executor.
  /**
   * \param[in] mode_yaml_map    Mapping from mode enum values to YAML file paths.
   * \param[in] mode_tester      Function that maps application state to the desired mode.
   * \param[in] initial_mode     The mode to activate at startup.
   * \param[in] initial_state    Initial value for the executor-owned state object.
   * \param[in] mode_offsets     Per-transition, per-callback offset delays (default empty).
   * \param[in] options          Executor options forwarded to the base class.
   * \throws std::invalid_argument if mode_yaml_map is empty, mode_tester is null,
   *         or initial_mode is not found in mode_yaml_map.
   */
  explicit ModeAwareExecutor(
    std::map<ModeEnumT, std::string> mode_yaml_map,
    ModeTesterFn mode_tester,
    ModeEnumT initial_mode,
    StateT initial_state = {},
    ModeOffsetMap mode_offsets = {},
    const rclcpp::ExecutorOptions & options = rclcpp::ExecutorOptions())
  : NoExecutor(options, nullptr),
    mode_tester_(std::move(mode_tester)),
    current_mode_(initial_mode),
    state_(std::move(initial_state)),
    mode_offsets_(std::move(mode_offsets))
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

      // Store chains via shared_ptr so ChainPriorityAllocator has shared ownership.
      mode_chains_[mode] = std::make_shared<std::unordered_map<std::string, rclcpp::userChain>>(
        parser.get_user_chains());

      mode_allocators_[mode] = std::make_shared<rclcpp::detail::ChainPriorityAllocator>(
        mode_chains_[mode]);
    }

    // Activate the initial mode
    this->chain_priority_allocator_ = mode_allocators_[initial_mode];
  }

  ~ModeAwareExecutor() override = default;

  /// Apply a state update and perform a mode change if the new state requires one.
  /**
   * The executor owns state_; state_updater mutates it, then mode_tester_ derives the
   * target mode. Classifies each callback per this protocol:
   *   a) Old-mode completed/aborted → SCHED_OTHER immediately
   *   b) Wholly new → FIFO + period immediately, timer held until MCR + Y_i
   *   c) Changed → new FIFO priority + period immediately, timer held until MCR + Y_i
   *   d) Unchanged → period applied immediately, delay_next if Z_i offset provided
   *
   * Hold clearing is handled by handle_timer auto-clear when now >= hold_until.
   *
   * \param[in] state_updater  Callable that mutates the executor-owned state object.
   */
  void modeUpdate(std::function<void(StateT &)> state_updater)
  {
    state_updater(state_);
    ModeEnumT target_mode = mode_tester_(state_);

    if (target_mode == current_mode_) {
      return;
    }

    const auto logger = rclcpp::get_logger("ModeAwareExecutor");

    auto alloc_it = mode_allocators_.find(target_mode);
    if (alloc_it == mode_allocators_.end()) {
      RCLCPP_ERROR(
        logger,
        "modeUpdate: target mode not found in mode_allocators_; ignoring switch");
      return;
    }

    // Atomically claim the transition slot — rejects concurrent MCRs
    bool expected = false;
    if (!transition_in_progress_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel))
    {
      RCLCPP_WARN(logger, "Mode transition already in progress; ignoring MCR");
      return;
    }

    ModeEnumT old_mode = current_mode_;
    RCLCPP_INFO(logger, "MCR: mode %d -> %d",
      static_cast<int>(old_mode), static_cast<int>(target_mode));

    // Record MCR timestamp
    struct timespec mcr_time;
    clock_gettime(CLOCK_MONOTONIC, &mcr_time);

    // Step 1: Collect all named callback entities from registered nodes
    auto named = this->collect_named_entities();
    if (named.groups_by_name.empty()) {
      RCLCPP_WARN(logger, "No named callbacks registered; applying mode switch directly");
      current_mode_ = target_mode;
      this->chain_priority_allocator_ = alloc_it->second;
      transition_in_progress_.store(false, std::memory_order_release);
      return;
    }

    // Step 2: Look up pre-computed allocations from cache (populated by apply_chain_priorities).
    auto old_cache_it = mode_allocation_cache_.find(old_mode);
    auto new_cache_it = mode_allocation_cache_.find(target_mode);
    if (old_cache_it == mode_allocation_cache_.end() ||
      new_cache_it == mode_allocation_cache_.end())
    {
      RCLCPP_ERROR(
        logger,
        "mode_allocation_cache_ not populated; call spin() before modeUpdate()");
      transition_in_progress_.store(false, std::memory_order_release);
      return;
    }
    const auto & old_allocation = *old_cache_it->second;
    const auto & new_allocation = *new_cache_it->second;

    // Step 3: Determine callback sets for each mode
    auto old_cbs = get_callback_names_for_mode(old_mode);
    auto new_cbs = get_callback_names_for_mode(target_mode);

    // Step 4: Look up offsets for this transition
    static const std::map<std::string, int64_t> empty_offsets;
    auto offset_it = mode_offsets_.find({old_mode, target_mode});
    const auto & offsets = (offset_it != mode_offsets_.end())
      ? offset_it->second : empty_offsets;

    // Step 5: Classify each callback and apply changes immediately
    for (const auto & [cb_name, entity] : named.entities_by_name) {
      if (!entity) {
        continue;
      }

      // ModeAwareExecutor only supports timer callbacks in chains.
      // Subscriptions receive their priority via message passing (BundledSubscription).
      if (this->get_timer_period(cb_name) == 0) {
        RCLCPP_WARN(
          logger,
          "  [UNSUPPORTED] '%s' is not a registered timer; chain YAML should only "
          "reference timer callbacks. Subscriptions are prioritized via message "
          "passing. Skipping sched/period changes.",
          cb_name.c_str());
        continue;
      }

      bool in_old = old_cbs.count(cb_name) > 0;
      bool in_new = new_cbs.count(cb_name) > 0;

      auto old_prio_it = old_allocation.callback_priorities.find(cb_name);
      auto new_prio_it = new_allocation.callback_priorities.find(cb_name);
      uint16_t old_prio = (old_prio_it != old_allocation.callback_priorities.end())
                            ? old_prio_it->second : 0;
      uint16_t new_prio = (new_prio_it != new_allocation.callback_priorities.end())
                            ? new_prio_it->second : 0;

      // Look up chain periods from both allocations.
      // TODO: confirm userChain.period unit — assumed nanoseconds (same as set_timer_period).
      auto old_period_it = old_allocation.callback_periods.find(cb_name);
      auto new_period_it = new_allocation.callback_periods.find(cb_name);
      int64_t old_period_ns = (old_period_it != old_allocation.callback_periods.end())
                                ? static_cast<int64_t>(old_period_it->second) : 0;
      int64_t new_period_ns = (new_period_it != new_allocation.callback_periods.end())
                                ? static_cast<int64_t>(new_period_it->second) : 0;

      int64_t offset_ns = 0;
      auto off_it = offsets.find(cb_name);
      if (off_it != offsets.end()) {
        offset_ns = off_it->second;
      }

      if (in_old && !in_new) {
        // (a) OLD-MODE COMPLETED / ABORTED: demote to best-effort immediately
        RCLCPP_INFO(logger, "  [OLD-ONLY] '%s' -> SCHED_OTHER", cb_name.c_str());
        apply_sched_attr_to_entity(entity, SCHED_OTHER, 0);

      } else if (!in_old && in_new) {
        // (b) WHOLLY NEW: apply FIFO + period immediately; timer held until MCR + Y_i
        if (offset_ns > 0) {
          int64_t mcr_ns = (int64_t)mcr_time.tv_sec * 1'000'000'000L + mcr_time.tv_nsec;
          this->set_timer_hold_until(cb_name, mcr_ns + offset_ns);
        }
        apply_sched_attr_to_entity(entity, SCHED_FIFO, new_prio);
        if (new_period_ns > 0) {
          this->set_timer_period(cb_name, new_period_ns);
        }
        RCLCPP_INFO(logger, "  [WHOLLY NEW] '%s' -> FIFO(%u), period %ld ns%s",
          cb_name.c_str(), new_prio, new_period_ns,
          offset_ns > 0 ? ", held until MCR+offset" : "");

      } else if (in_old && in_new && old_prio != new_prio) {
        // (c) CHANGED: apply new priority + period immediately; timer held until MCR + Y_i
        if (offset_ns > 0) {
          int64_t mcr_ns = (int64_t)mcr_time.tv_sec * 1'000'000'000L + mcr_time.tv_nsec;
          this->set_timer_hold_until(cb_name, mcr_ns + offset_ns);
        }
        apply_sched_attr_to_entity(entity, SCHED_FIFO, new_prio);
        if (new_period_ns > 0) {
          this->set_timer_period(cb_name, new_period_ns);
        }
        RCLCPP_INFO(logger, "  [CHANGED] '%s' FIFO(%u) -> FIFO(%u), period %ld ns%s",
          cb_name.c_str(), old_prio, new_prio, new_period_ns,
          offset_ns > 0 ? ", held until MCR+offset" : "");

      } else if (in_old && in_new && old_prio == new_prio) {
        // (d) UNCHANGED priority: apply period immediately, delay_next if Z_i offset
        bool period_changed = (new_period_ns > 0 && new_period_ns != old_period_ns);
        if (offset_ns > 0) {
          this->set_timer_delay_next(cb_name, offset_ns);
        }
        if (period_changed) {
          this->set_timer_period(cb_name, new_period_ns);
        }
        if (!period_changed && offset_ns == 0) {
          RCLCPP_DEBUG(logger, "  [UNCHANGED] '%s' FIFO(%u) -- no action", cb_name.c_str(), old_prio);
        } else {
          RCLCPP_INFO(logger, "  [UNCHANGED] '%s' FIFO(%u), period %ld -> %ld ns%s",
            cb_name.c_str(), old_prio, old_period_ns,
            period_changed ? new_period_ns : old_period_ns,
            offset_ns > 0 ? ", delay_next applied" : "");
        }
      }
      // else: not in either mode's chains — skip
    }

    // Step 6: Update active allocator and current mode
    current_mode_ = target_mode;
    this->chain_priority_allocator_ = alloc_it->second;

    RCLCPP_INFO(logger, "Mode switch complete: %d -> %d",
      static_cast<int>(old_mode), static_cast<int>(target_mode));
    transition_in_progress_.store(false, std::memory_order_release);
  }

  /// Invalidate the allocation cache when nodes are added/removed so
  /// apply_chain_priorities() rebuilds it with the updated callback set.
  void add_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify = true) override
  {
    NoExecutor::add_node(node_ptr, notify);
    mode_allocation_cache_.clear();
  }

  void remove_node(std::shared_ptr<rclcpp::Node> node_ptr, bool notify = true) override
  {
    NoExecutor::remove_node(node_ptr, notify);
    mode_allocation_cache_.clear();
  }

  /// Return the currently active mode.
  ModeEnumT get_current_mode() const
  {
    return current_mode_;
  }

  /// Return whether a mode transition is currently in progress.
  bool is_transition_in_progress() const
  {
    return transition_in_progress_.load();
  }

protected:
  /// Pre-compute priority allocations for ALL modes as soon as nodes are registered.
  /// Called by NoExecutor::spin() before the event loop starts.
  void apply_chain_priorities() override
  {
    // Apply initial mode priorities via base class
    NoExecutor::apply_chain_priorities();

    // Eagerly populate the cache for every mode so modeUpdate() never computes allocations
    auto named = this->collect_named_entities();
    for (auto & [mode, allocator] : mode_allocators_) {
      if (mode_allocation_cache_.find(mode) == mode_allocation_cache_.end()) {
        mode_allocation_cache_[mode] =
          std::make_shared<rclcpp::detail::ChainPriorityAllocation>(
          allocator->allocate(named.groups_by_name));
      }
    }
  }

private:
  /// Get all callback names referenced in any chain for a given mode.
  std::unordered_set<std::string> get_callback_names_for_mode(ModeEnumT mode) const
  {
    std::unordered_set<std::string> names;
    auto it = mode_chains_.find(mode);
    if (it == mode_chains_.end()) {
      return names;
    }
    for (const auto & [chain_name, chain] : *it->second) {
      for (const auto & cb : chain.callbacks) {
        names.insert(cb);
      }
    }
    return names;
  }

  // mode_chains_ uses shared_ptr values, so ChainPriorityAllocator shares ownership.
  std::map<ModeEnumT,
    std::shared_ptr<std::unordered_map<std::string, rclcpp::userChain>>> mode_chains_;
  std::map<ModeEnumT, std::shared_ptr<rclcpp::detail::ChainPriorityAllocator>> mode_allocators_;
  std::map<ModeEnumT, std::shared_ptr<rclcpp::detail::ChainPriorityAllocation>>
    mode_allocation_cache_;

  ModeTesterFn mode_tester_;
  ModeEnumT current_mode_;
  StateT state_;

  ModeOffsetMap mode_offsets_;
  std::atomic<bool> transition_in_progress_{false};
};

}  // namespace executors
}  // namespace rclcpp

#endif  // RCLCPP__EXECUTORS__MODE_AWARE_EXECUTOR_HPP_
