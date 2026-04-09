#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace core {

struct EntityId {
    static constexpr std::uint32_t kInvalidIndex = static_cast<std::uint32_t>(-1);

    std::uint32_t index{kInvalidIndex};
    std::uint32_t generation{0};

    [[nodiscard]] bool valid() const {
        return index != kInvalidIndex;
    }

    friend bool operator==(EntityId lhs, EntityId rhs) = default;
};

}  // namespace core

template <>
struct std::hash<core::EntityId> {
    std::size_t operator()(const core::EntityId& entity) const noexcept {
        return (static_cast<std::size_t>(entity.index) << 32u) ^ entity.generation;
    }
};
