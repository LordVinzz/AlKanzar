#pragma once

#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "Entity.hpp"

namespace core {

template <typename T>
class ComponentStore {
public:
    static constexpr std::size_t kInvalidIndex = std::numeric_limits<std::size_t>::max();

    template <typename... Args>
    T& emplace(EntityId entity, Args&&... args) {
        if (contains(entity)) {
            T& component = get(entity);
            component = T(std::forward<Args>(args)...);
            return component;
        }

        ensureSparseSize(entity.index + 1u);
        sparse_[entity.index] = denseEntities_.size();
        denseEntities_.push_back(entity);
        denseComponents_.emplace_back(std::forward<Args>(args)...);
        return denseComponents_.back();
    }

    bool contains(EntityId entity) const {
        if (!entity.valid() || entity.index >= sparse_.size()) {
            return false;
        }

        const std::size_t denseIndex = sparse_[entity.index];
        return denseIndex != kInvalidIndex &&
               denseIndex < denseEntities_.size() &&
               denseEntities_[denseIndex] == entity;
    }

    T* tryGet(EntityId entity) {
        return const_cast<T*>(std::as_const(*this).tryGet(entity));
    }

    const T* tryGet(EntityId entity) const {
        if (!contains(entity)) {
            return nullptr;
        }
        return &denseComponents_[sparse_[entity.index]];
    }

    T& get(EntityId entity) {
        return denseComponents_[sparse_.at(entity.index)];
    }

    const T& get(EntityId entity) const {
        return denseComponents_[sparse_.at(entity.index)];
    }

    void remove(EntityId entity) {
        if (!contains(entity)) {
            return;
        }

        const std::size_t denseIndex = sparse_[entity.index];
        const std::size_t lastIndex = denseEntities_.size() - 1u;

        if (denseIndex != lastIndex) {
            denseEntities_[denseIndex] = denseEntities_[lastIndex];
            denseComponents_[denseIndex] = std::move(denseComponents_[lastIndex]);
            sparse_[denseEntities_[denseIndex].index] = denseIndex;
        }

        denseEntities_.pop_back();
        denseComponents_.pop_back();
        sparse_[entity.index] = kInvalidIndex;
    }

    void clear() {
        denseEntities_.clear();
        denseComponents_.clear();
        sparse_.clear();
    }

    [[nodiscard]] std::size_t size() const {
        return denseEntities_.size();
    }

    [[nodiscard]] const std::vector<EntityId>& entities() const {
        return denseEntities_;
    }

    [[nodiscard]] const std::vector<T>& values() const {
        return denseComponents_;
    }

    [[nodiscard]] std::vector<T>& values() {
        return denseComponents_;
    }

private:
    void ensureSparseSize(std::size_t size) {
        if (sparse_.size() < size) {
            sparse_.resize(size, kInvalidIndex);
        }
    }

    std::vector<EntityId> denseEntities_;
    std::vector<T> denseComponents_;
    std::vector<std::size_t> sparse_;
};

}  // namespace core
