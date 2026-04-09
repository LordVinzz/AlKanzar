#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace core {

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    std::size_t connect(Slot slot) {
        const std::size_t id = nextId_++;
        slots_.push_back(SlotRecord{id, std::move(slot), true});
        return id;
    }

    void disconnect(std::size_t id) {
        for (auto& slot : slots_) {
            if (slot.id == id) {
                slot.active = false;
                break;
            }
        }
    }

    void notify(Args... args) {
        for (auto& slot : slots_) {
            if (slot.active && slot.callback) {
                slot.callback(args...);
            }
        }
        compact();
    }

    [[nodiscard]] std::size_t slotCount() const {
        return slots_.size();
    }

private:
    struct SlotRecord {
        std::size_t id{0};
        Slot callback{};
        bool active{false};
    };

    void compact() {
        slots_.erase(
            std::remove_if(
                slots_.begin(),
                slots_.end(),
                [](const SlotRecord& slot) { return !slot.active; }
            ),
            slots_.end()
        );
    }

    std::size_t nextId_{1};
    std::vector<SlotRecord> slots_;
};

}  // namespace core
