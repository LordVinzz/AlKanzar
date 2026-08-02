#include "core/navigation/Navigation.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace core {

using namespace navigation_detail;

const char* navSourceTagName(NavSourceTag tag) {
    switch (tag) {
        case NavSourceTag::Walkable:
            return "Walkable";
        case NavSourceTag::Blocking:
            return "Blocking";
        case NavSourceTag::Ignored:
        default:
            return "Ignored";
    }
}

bool tryParseNavSourceTag(const std::string& token, NavSourceTag& outTag) {
    const std::string lowered = lowercaseCopy(token);
    if (lowered == "walkable") {
        outTag = NavSourceTag::Walkable;
        return true;
    }
    if (lowered == "blocking") {
        outTag = NavSourceTag::Blocking;
        return true;
    }
    if (lowered == "ignored") {
        outTag = NavSourceTag::Ignored;
        return true;
    }
    return false;
}

std::string serializeNavMeshAsset(const NavMeshAsset& asset) {
    std::ostringstream out;
    // max_digits10 guarantees an exact float round-trip. Four decimal places
    // were enough for display, but could move nearly-collinear navmesh edges
    // far enough to create sliver portals after a save/reload cycle.
    out << std::fixed << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "version " << asset.version << "\n";
    out << "minimum_runtime_cell_area "
        << std::max(asset.minimumRuntimeCellArea, 0.0f) << "\n";
    for (const NavSourceTagOverride& overrideRecord : asset.sourceTagOverrides) {
        out << "source_tag_override " << std::quoted(overrideRecord.stableId) << " " << navSourceTagName(overrideRecord.tag) << "\n";
    }
    for (const NavPolygon& polygon : asset.polygons) {
        out << "polygon " << polygon.id << " " << polygon.elevationY;
        for (const glm::vec2& vertex : polygon.verticesXZ) {
            out << " " << vertex.x << " " << vertex.y;
        }
        out << "\n";
    }
    for (const NavLink& link : asset.links) {
        out << "link " << link.id << " " << link.fromPolygonId << " " << link.toPolygonId
            << " "
            << link.fromPoint.x << " " << link.fromPoint.y << " " << link.fromPoint.z
            << " " << link.toPoint.x << " " << link.toPoint.y << " " << link.toPoint.z
            << " " << (link.bidirectional ? 1 : 0) << "\n";
    }
    return out.str();
}

bool parseNavMeshAsset(const std::string& text, NavMeshAsset& outAsset, std::string* error) {
    std::istringstream input(text);
    std::string line{};
    NavMeshAsset asset{};
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream lineStream(line);
        std::string keyword{};
        lineStream >> keyword;
        if (keyword == "version") {
            if (!(lineStream >> asset.version)) {
                if (error) {
                    *error = "Invalid version at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "minimum_runtime_cell_area") {
            if (!(lineStream >> asset.minimumRuntimeCellArea) ||
                !std::isfinite(asset.minimumRuntimeCellArea) ||
                asset.minimumRuntimeCellArea < 0.0f) {
                if (error) {
                    *error = "Invalid minimum_runtime_cell_area at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "source_tag_override") {
            std::string stableId{};
            std::string tagToken{};
            if (!(lineStream >> std::quoted(stableId) >> tagToken)) {
                if (error) {
                    *error = "Invalid source_tag_override at line " + std::to_string(lineNumber);
                }
                return false;
            }
            NavSourceTag tag = NavSourceTag::Ignored;
            if (!tryParseNavSourceTag(tagToken, tag)) {
                if (error) {
                    *error = "Unknown nav tag at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.sourceTagOverrides.push_back(NavSourceTagOverride{stableId, tag});
        } else if (keyword == "polygon") {
            NavPolygon polygon{};
            if (!(lineStream >> polygon.id >> polygon.elevationY)) {
                if (error) {
                    *error = "Invalid polygon header at line " + std::to_string(lineNumber);
                }
                return false;
            }
            glm::vec2 vertex(0.0f);
            while (lineStream >> vertex.x >> vertex.y) {
                polygon.verticesXZ.push_back(vertex);
            }
            if (!polygonValid(polygon)) {
                if (error) {
                    *error = "Polygon must have at least three vertices at line " + std::to_string(lineNumber);
                }
                return false;
            }
            asset.polygons.push_back(std::move(polygon));
        } else if (keyword == "link") {
            NavLink link{};
            int bidirectional = 0;
            if (!(lineStream >> link.id >> link.fromPolygonId >> link.toPolygonId
                    >> link.fromPoint.x >> link.fromPoint.y >> link.fromPoint.z
                    >> link.toPoint.x >> link.toPoint.y >> link.toPoint.z
                    >> bidirectional)) {
                if (error) {
                    *error = "Invalid link at line " + std::to_string(lineNumber);
                }
                return false;
            }
            link.bidirectional = bidirectional != 0;
            asset.links.push_back(link);
        } else {
            if (error) {
                *error = "Unknown keyword '" + keyword + "' at line " + std::to_string(lineNumber);
            }
            return false;
        }
    }

    if (asset.version != kNavAssetVersion) {
        if (error) {
            *error = "Unsupported navmesh version " + std::to_string(asset.version);
        }
        return false;
    }

    outAsset = std::move(asset);
    return true;
}

}  // namespace core
