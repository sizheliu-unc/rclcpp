#pragma once

#include <atomic>

namespace syncutil {

template<class T>
struct StackNode
{
    T* data;
    StackNode* next;
    StackNode(T* data) : data(data), next(nullptr) {}
};

template<class T>
class StackAtomic
{
public:
    StackAtomic():head(nullptr) {}
    void push(T* data)
    {
        // const std::lock_guard<std::mutex> lock(mutex_);
        StackNode<T>* new_node = new StackNode<T>(data);
        new_node->next = head.load(std::memory_order_relaxed);
        // head.store(new_node);
        while (!head.compare_exchange_weak(
                    new_node->next, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed));
    }

    bool is_empty() {
        // const std::lock_guard<std::mutex> lock(mutex_);
        return head.load(std::memory_order_acquire) == nullptr;
    }

    T* pop() {
        // const std::lock_guard<std::mutex> lock(mutex_);
        StackNode<T>* cur_head = head.load(std::memory_order_acquire);

        // if (cur_head == nullptr) return nullptr;
        // head.store(cur_head->next);

        while(cur_head != nullptr
                && !head.compare_exchange_weak(
                        cur_head,
                        cur_head->next,
                        std::memory_order_release,
                        std::memory_order_relaxed));
        if (cur_head == nullptr) {
            return nullptr;
        }
        T* data = cur_head->data;
        delete cur_head;
        return data;
    }

private:
    std::atomic<StackNode<T>*> head;
    std::mutex mutex_;
};

}

