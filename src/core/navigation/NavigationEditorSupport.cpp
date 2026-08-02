#include "core/navigation/NavigationDetailEditor.hpp"

#include <algorithm>

namespace core::navigation_detail {

int nextPolygonId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavPolygon& polygon : asset.polygons) {
        nextId = std::max(nextId, polygon.id + 1);
    }
    return nextId;
}

int nextLinkId(const NavMeshAsset& asset) {
    int nextId = 1;
    for (const NavLink& link : asset.links) {
        nextId = std::max(nextId, link.id + 1);
    }
    return nextId;
}

void updateSourceOverride(
    std::vector<NavSourceTagOverride>& overrides,
    const NavSourceComponent& source,
    NavSourceTag appliedTag
) {
    const auto it = std::find_if(overrides.begin(), overrides.end(), [&source](const NavSourceTagOverride& overrideRecord) {
        return overrideRecord.stableId == source.stableId;
    });
    if (appliedTag == source.defaultTag) {
        if (it != overrides.end()) {
            overrides.erase(it);
        }
        return;
    }

    if (it != overrides.end()) {
        it->tag = appliedTag;
        return;
    }
    overrides.push_back(NavSourceTagOverride{source.stableId, appliedTag});
}

}  // namespace core::navigation_detail

