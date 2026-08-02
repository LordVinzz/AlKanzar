#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/navigation/Navigation.hpp"

namespace core::navigation_detail {

int nextPolygonId(const NavMeshAsset& asset);
int nextLinkId(const NavMeshAsset& asset);
std::optional<std::size_t> findPolygonIndexById(const NavigationRuntime& runtime, int polygonId);
void updateSourceOverride(
    std::vector<NavSourceTagOverride>& overrides,
    const NavSourceComponent& source,
    NavSourceTag appliedTag
);

}  // namespace core::navigation_detail
