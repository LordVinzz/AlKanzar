#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <variant>
#include <vector>

namespace core {

template <typename TVariant>
class EventBus {
public:
    explicit EventBus(std::size_t reserve = 256u) {
        queue_.reserve(reserve);
    }

    template <typename TEvent>
    std::size_t subscribe(std::function<void(const TEvent&)> handler) {
        const std::size_t id = nextSubscriptionId_++;
        subscriptions_.push_back(Subscription{
            id,
            [handler = std::move(handler)](const TVariant& event) {
                if (const auto* typed = std::get_if<TEvent>(&event)) {
                    handler(*typed);
                }
            },
            true
        });
        return id;
    }

    void unsubscribe(std::size_t id) {
        for (auto& subscription : subscriptions_) {
            if (subscription.id == id) {
                subscription.active = false;
                break;
            }
        }
    }

    template <typename TEvent>
    void publish(TEvent event) {
        queue_.emplace_back(std::move(event));
    }

    void dispatch() {
        for (std::size_t index = 0; index < queue_.size(); ++index) {
            const TVariant& event = queue_[index];
            for (auto& subscription : subscriptions_) {
                if (subscription.active) {
                    subscription.handler(event);
                }
            }
        }
        queue_.clear();
        compact();
    }

    void clear() {
        queue_.clear();
    }

    [[nodiscard]] std::size_t pendingCount() const {
        return queue_.size();
    }

private:
    struct Subscription {
        std::size_t id{0};
        std::function<void(const TVariant&)> handler{};
        bool active{false};
    };

    void compact() {
        subscriptions_.erase(
            std::remove_if(
                subscriptions_.begin(),
                subscriptions_.end(),
                [](const Subscription& subscription) { return !subscription.active; }
            ),
            subscriptions_.end()
        );
    }

    std::size_t nextSubscriptionId_{1};
    std::vector<TVariant> queue_;
    std::vector<Subscription> subscriptions_;
};

}  // namespace core
