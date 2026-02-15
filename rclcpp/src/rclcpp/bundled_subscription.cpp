#include "rclcpp/bundled_subscription.hpp"
#include "rclcpp/exceptions.hpp"

template <typename Taker>
static void
try_take(
    const char *action_description,
    const char *topic_or_service_name,
    Taker take_action)
{
    try
    {
        take_action();
    }
    catch (const rclcpp::exceptions::RCLError &rcl_error)
    {
        RCLCPP_ERROR(
            rclcpp::get_logger("rclcpp"),
            "executor %s '%s' unexpectedly failed: %s",
            action_description,
            topic_or_service_name,
            rcl_error.what());
    }
}

namespace rclcpp
{
    BundledSubscription::BundledSubscription(rclcpp::SubscriptionBase::SharedPtr subscription_) : subscription(subscription_)
    {
        message_info.get_rmw_message_info().from_intra_process = false;
    }

    rclcpp::SubscriptionBase::SharedPtr BundledSubscription::get() const
    {
        return subscription;
    }

    LoanedMsgSubscription::LoanedMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription_) : BundledSubscription(subscription_) {}

    std::unique_ptr<BundledSubscription> LoanedMsgSubscription::take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription)
    {
        auto bundle = std::make_unique<LoanedMsgSubscription>(subscription);
        try_take(
            "taking a loaned message from topic",
            bundle->subscription->get_topic_name(),
            [&]()
            {
                rcl_ret_t ret = rcl_take_loaned_message(
                    subscription->get_subscription_handle().get(),
                    &bundle->loaned_msg,
                    &bundle->message_info.get_rmw_message_info(),
                    nullptr);
                if (RCL_RET_SUBSCRIPTION_TAKE_FAILED == ret)
                {
                    return false;
                }
                else if (RCL_RET_OK != ret)
                {
                    rclcpp::exceptions::throw_from_rcl_error(ret);
                }
                return true;
            });

        if (bundle->loaned_msg == nullptr)
        {
            return nullptr;
        }

        return bundle;
    }

    uint32_t LoanedMsgSubscription::get_message_prio() const
    {
        return subscription->get_loaned_message_prio(loaned_msg);
    }

    void LoanedMsgSubscription::run()
    {
        if (loaned_msg == nullptr)
        {
            RCLCPP_ERROR(
                rclcpp::get_logger("rclcpp"),
                "executor %s '%s' unexpectedly failed: %s",
                "LoanedMsgSubscription run called twice",
                subscription->get_topic_name(),
                "LoanedMsgSubscription run called twice");
        }

        subscription->handle_loaned_message(loaned_msg, message_info);

        rcl_ret_t ret = rcl_return_loaned_message_from_subscription(
            subscription->get_subscription_handle().get(), loaned_msg);
        if (RCL_RET_OK != ret)
        {
            RCLCPP_ERROR(
                rclcpp::get_logger("rclcpp"),
                "rcl_return_loaned_message_from_subscription() failed for subscription on topic "
                "'%s': %s",
                subscription->get_topic_name(), rcl_get_error_string().str);
        }
        loaned_msg = nullptr;
    }

    GenericMsgSubscription::GenericMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription_) : BundledSubscription(subscription_), message(subscription_->create_message()) {}

    std::unique_ptr<BundledSubscription> GenericMsgSubscription::take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription)
    {
        auto bundle = std::make_unique<GenericMsgSubscription>(subscription);
        try_take(
            "taking a message from topic",
            subscription->get_topic_name(),
            [&]()
            { return subscription->take_type_erased(bundle->message.get(), bundle->message_info); });

        if (bundle->message == nullptr)
        {
            return nullptr;
        }

        return bundle;
    }

    uint32_t GenericMsgSubscription::get_message_prio() const
    {
        return subscription->get_message_prio(message);
    }

    void GenericMsgSubscription::run()
    {
        if (message == nullptr)
        {
            RCLCPP_ERROR(
                rclcpp::get_logger("rclcpp"),
                "executor %s '%s' unexpectedly failed: %s",
                "GenericMsgSubscription run called twice",
                subscription->get_topic_name(),
                "GenericMsgSubscription run called twice");
        }

        subscription->handle_message(message, message_info);

        subscription->return_message(message);
    }

    SerializedMsgSubscription::SerializedMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription_) : BundledSubscription(subscription_), serialized_msg(subscription_->create_serialized_message()) {}

    std::unique_ptr<BundledSubscription> SerializedMsgSubscription::take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription)
    {
        auto bundle = std::make_unique<SerializedMsgSubscription>(subscription);
        try_take(
            "taking a serialized message from topic",
            subscription->get_topic_name(),
            [&]()
            { return subscription->take_serialized(*bundle->serialized_msg.get(), bundle->message_info); });

        if (bundle->serialized_msg == nullptr)
        {
            return nullptr;
        }

        return bundle;
    }

    uint32_t SerializedMsgSubscription::get_message_prio() const
    {
        RCLCPP_ERROR(
            rclcpp::get_logger("rclcpp"),
            "executor %s '%s' unexpectedly failed: %s",
            "get_message_prio not supported for serialized subscription",
            subscription->get_topic_name(),
            "get_message_prio not supported for serialized subscription");
        return 0;
    }

    void SerializedMsgSubscription::run()
    {
        if (serialized_msg == nullptr)
        {
            RCLCPP_ERROR(
                rclcpp::get_logger("rclcpp"),
                "executor %s '%s' unexpectedly failed: %s",
                "SerializedMsgSubscription run called twice",
                subscription->get_topic_name(),
                "SerializedMsgSubscription run called twice");
        }

        subscription->handle_serialized_message(serialized_msg, message_info);

        subscription->return_serialized_message(serialized_msg);
    }

    std::unique_ptr<BundledSubscription> take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription)
    {
        if (!subscription->is_serialized())
        {
            if (subscription->can_loan_messages())
            {
                return LoanedMsgSubscription::take_and_bundle(subscription);
            }
            else
            {
                return GenericMsgSubscription::take_and_bundle(subscription);
            }
        }
        else
        {
            return SerializedMsgSubscription::take_and_bundle(subscription);
        }
    }
}