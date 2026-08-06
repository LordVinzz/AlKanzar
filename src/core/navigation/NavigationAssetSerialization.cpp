#include "core/navigation/Navigation.hpp"
#include "core/content/ContentFileHeader.hpp"
#include "core/navigation/NavigationDetailGeometry.hpp"
#include "core/navigation/NavigationDetailPolygon.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace core {

using namespace navigation_detail;

namespace {

constexpr std::string_view kNavContentType = "NAV";

}  // namespace

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

std::string serializeNavMeshAsset(const NavMeshAsset& asset, std::string* error) {
    if (asset.version != kNavAssetVersion) {
        if (error != nullptr) {
            *error = "Cannot serialize unsupported navmesh version " +
                std::to_string(asset.version) + ".";
        }
        return {};
    }

    std::array<char, kContentFileHeaderSize> header{};
    if (!encodeContentFileHeader(
            static_cast<std::uint32_t>(asset.version),
            kNavContentType,
            header,
            error)) {
        return {};
    }

    std::ostringstream out;
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    // max_digits10 guarantees an exact float round-trip. Four decimal places
    // were enough for display, but could move nearly-collinear navmesh edges
    // far enough to create sliver portals after a save/reload cycle.
    out << std::fixed << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "minimum_runtime_cell_area "
        << std::max(asset.minimumRuntimeCellArea, 0.0f) << "\n";
    out << "maximum_polygon_edge_length "
        << std::max(asset.maximumPolygonEdgeLength, 0.0f) << "\n";
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
    if (error != nullptr) {
        error->clear();
    }
    return out.str();
}

bool parseNavMeshAsset(const std::string& text, NavMeshAsset& outAsset, std::string* error) {
    const bool hasVersionedHeader = !text.empty() && text.front() == 'V';
    std::string_view payload = text;
    ContentFileHeader fileHeader{};
    if (hasVersionedHeader) {
        if (!decodeContentFileHeader(text, fileHeader, error)) {
            return false;
        }
        if (fileHeader.type != kNavContentType) {
            if (error != nullptr) {
                *error = "Expected content type NAV, got " + fileHeader.type + ".";
            }
            return false;
        }
        payload.remove_prefix(kContentFileHeaderSize);
    }

    std::istringstream input{std::string(payload)};
    std::string line{};
    NavMeshAsset asset{};
    bool payloadContainsVersion = false;
    if (hasVersionedHeader) {
        asset.version = static_cast<int>(fileHeader.version);
    }
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
            int payloadVersion = 0;
            if (!(lineStream >> payloadVersion)) {
                if (error) {
                    *error = "Invalid version at line " + std::to_string(lineNumber);
                }
                return false;
            }
            if (hasVersionedHeader && payloadVersion != asset.version) {
                if (error != nullptr) {
                    *error = "Header and payload versions differ at line " +
                        std::to_string(lineNumber);
                }
                return false;
            }
            asset.version = payloadVersion;
            payloadContainsVersion = true;
        } else if (keyword == "minimum_runtime_cell_area") {
            if (!(lineStream >> asset.minimumRuntimeCellArea) ||
                !std::isfinite(asset.minimumRuntimeCellArea) ||
                asset.minimumRuntimeCellArea < 0.0f) {
                if (error) {
                    *error = "Invalid minimum_runtime_cell_area at line " + std::to_string(lineNumber);
                }
                return false;
            }
        } else if (keyword == "maximum_polygon_edge_length") {
            if (!(lineStream >> asset.maximumPolygonEdgeLength) ||
                !std::isfinite(asset.maximumPolygonEdgeLength) ||
                asset.maximumPolygonEdgeLength < 0.0f) {
                if (error) {
                    *error = "Invalid maximum_polygon_edge_length at line " +
                        std::to_string(lineNumber);
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

    if (!hasVersionedHeader && !payloadContainsVersion) {
        if (error != nullptr) {
            *error = "Legacy navmesh asset has no version line.";
        }
        return false;
    }

    if (asset.version != kNavAssetVersion) {
        if (error) {
            *error = "Unsupported navmesh version " + std::to_string(asset.version);
        }
        return false;
    }

    outAsset = std::move(asset);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace core
