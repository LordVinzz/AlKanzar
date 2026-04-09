#pragma once

#include <cstdint>
#include <vector>

#include "Entity.hpp"

namespace core {

class EntityPool {
public:
    [[nodiscard]] EntityId create() {
        if (!freeList_.empty()) {
            const std::uint32_t index = freeList_.back();
            freeList_.pop_back();
            alive_[index] = true;
            return EntityId{index, generations_[index]};
        }

        const std::uint32_t index = static_cast<std::uint32_t>(generations_.size());
        generations_.push_back(1);
        alive_.push_back(true);
        return EntityId{index, generations_.back()};
    }

    void destroy(EntityId entity) {
        if (!isAlive(entity)) {
            return;
        }

        alive_[entity.index] = false;
        ++generations_[entity.index];
        freeList_.push_back(entity.index);
    }

    [[nodiscard]] bool isAlive(EntityId entity) const {
        return entity.valid() &&
               entity.index < generations_.size() &&
               alive_[entity.index] &&
               generations_[entity.index] == entity.generation;
    }

    void clear() {
        generations_.clear();
        alive_.clear();
        freeList_.clear();
    }

    [[nodiscard]] std::size_t capacity() const {
        return generations_.size();
    }

private:
    std::vector<std::uint32_t> generations_;
    std::vector<bool> alive_;
    std::vector<std::uint32_t> freeList_;
};

}  // namespace core
