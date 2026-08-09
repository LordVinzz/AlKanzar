#pragma once

#include <cstddef>

namespace core {

class World;

namespace navigation_detail {

[[nodiscard]] std::size_t applyAgentLocalAvoidance(World& world);

}  // namespace navigation_detail
}  // namespace core
