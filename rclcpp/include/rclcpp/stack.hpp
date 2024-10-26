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
        StackNode<T>* new_node = new StackNode<T>(data);
        new_node->next = head.load(std::memory_order_relaxed);
        while (!head.compare_exchange_weak(
                    new_node->next, new_node,
                    std::memory_order_release,
                    std::memory_order_relaxed));
    }

    bool is_empty() {
        return head.load(std::memory_order_acquire) == nullptr;
    }

    T* pop() {
        StackNode<T>* cur_head = head.load(std::memory_order_acquire);
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
};

}

