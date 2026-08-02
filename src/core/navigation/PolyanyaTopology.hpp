#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "Navigation.hpp"
#include "polyanya/mesh.h"

namespace core::navigation_detail {

inline constexpr double kTopologyEpsilon = 1.0e-4;
inline constexpr double kWeldEpsilon = 2.5e-5;
inline constexpr double kWeldQuantum = kWeldEpsilon;
inline constexpr double kJunctionSnapEpsilon = 8.0e-4;
inline constexpr double kSearchEpsilon = 1.0e-8;

struct WeldKey {
    std::size_t component{0u};
    std::int64_t x{0};
    std::int64_t y{0};

    friend bool operator==(const WeldKey&, const WeldKey&) = default;
};

struct WeldKeyHash {
    std::size_t operator()(const WeldKey& key) const noexcept;
};

struct UndirectedEdgeKey {
    int low{-1};
    int high{-1};

    friend bool operator==(const UndirectedEdgeKey&, const UndirectedEdgeKey&) = default;
};

struct UndirectedEdgeKeyHash {
    std::size_t operator()(const UndirectedEdgeKey& key) const noexcept;
};

struct EdgeOccurrence {
    std::size_t cell{0u};
    std::size_t neighbourSlot{0u};
};

[[nodiscard]] double cross(const polyanya::Point& lhs, const polyanya::Point& rhs);
[[nodiscard]] double signedArea(const std::vector<polyanya::Point>& polygon);
[[nodiscard]] bool isConvexCounterClockwise(const std::vector<polyanya::Point>& polygon);
[[nodiscard]] bool pointOnSegment(
    const polyanya::Point& point,
    const polyanya::Point& a,
    const polyanya::Point& b,
    double tolerance = kTopologyEpsilon
);
[[nodiscard]] double segmentParameter(
    const polyanya::Point& point,
    const polyanya::Point& a,
    const polyanya::Point& b
);
[[nodiscard]] polyanya::Point interpolate(
    const polyanya::Point& a,
    const polyanya::Point& b,
    double t
);
[[nodiscard]] bool graphContainsPortal(
    const std::vector<std::vector<NavGraphEdge>>& graph,
    std::size_t fromCell,
    std::size_t toCell,
    const polyanya::Point& midpoint,
    std::vector<std::vector<std::uint8_t>>* matchedPortals
);
[[nodiscard]] std::vector<std::size_t> planarComponents(
    std::size_t cellCount,
    const std::vector<std::vector<NavGraphEdge>>& graph
);

class PolyanyaMesh {
public:
    polyanya::Mesh mesh{};
    std::vector<float> elevations{};
    std::vector<std::size_t> components{};
};

}  // namespace core::navigation_detail
