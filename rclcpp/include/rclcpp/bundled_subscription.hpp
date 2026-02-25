#ifndef RCLCPP__BUNDLED_SUB
#define RCLCPP__BUNDLED_SUB

#include "rclcpp/subscription_base.hpp"

namespace rclcpp
{
    class BundledSubscription
    {
    protected:
        rclcpp::SubscriptionBase::SharedPtr subscription;
        rclcpp::MessageInfo message_info;
        BundledSubscription(rclcpp::SubscriptionBase::SharedPtr subscription);

    public:
        rclcpp::SubscriptionBase::SharedPtr get() const;
        virtual void run() = 0;
        virtual uint32_t get_message_prio() const = 0;
        virtual ~BundledSubscription() = default;
    };

    class LoanedMsgSubscription : public BundledSubscription
    {
    protected:
        void *loaned_msg;
        
    public:
        LoanedMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription);
        static std::unique_ptr<BundledSubscription> take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription);
        void run() override;
        virtual uint32_t get_message_prio() const override;
    };

    class GenericMsgSubscription : public BundledSubscription
    {
    protected:
        std::shared_ptr<void> message;
        
    public:
        GenericMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription);
        static std::unique_ptr<BundledSubscription> take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription);
        void run() override;
        virtual uint32_t get_message_prio() const override;
    };

    class SerializedMsgSubscription : public BundledSubscription
    {
    protected:
        std::shared_ptr<rclcpp::SerializedMessage> serialized_msg;
        uint32_t prio;

    public:
        SerializedMsgSubscription(rclcpp::SubscriptionBase::SharedPtr subscription);
        static std::unique_ptr<BundledSubscription> take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription);
        void run() override;
        virtual uint32_t get_message_prio() const override;
    };

    std::unique_ptr<BundledSubscription> take_and_bundle(rclcpp::SubscriptionBase::SharedPtr subscription);
}

#endif