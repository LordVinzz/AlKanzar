#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace core {

inline constexpr std::size_t kContentFileHeaderSize = 10u;

struct ContentFileHeader {
    std::uint32_t version{0u};
    std::string type{};

    friend bool operator==(const ContentFileHeader&, const ContentFileHeader&) = default;
};

namespace content_header_detail {

inline bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

inline bool validType(std::string_view type) {
    if (type.empty()) {
        return false;
    }
    for (const char character : type) {
        if (character < 'A' || character > 'Z') {
            return false;
        }
    }
    return true;
}

}  // namespace content_header_detail

// The fixed-width header is an ASCII token followed by zero padding:
// V<decimal version><uppercase type>\0...  For example, NAV version 1 is
// encoded as the ten bytes "V1NAV\0\0\0\0\0".
inline bool encodeContentFileHeader(
    std::uint32_t version,
    std::string_view type,
    std::array<char, kContentFileHeaderSize>& outHeader,
    std::string* error = nullptr
) {
    if (version == 0u) {
        return content_header_detail::fail(error, "Content version must be greater than zero.");
    }
    if (!content_header_detail::validType(type)) {
        return content_header_detail::fail(
            error,
            "Content type must contain only uppercase ASCII letters."
        );
    }

    std::array<char, kContentFileHeaderSize> header{};
    header[0] = 'V';
    const auto versionResult = std::to_chars(
        header.data() + 1,
        header.data() + header.size(),
        version
    );
    if (versionResult.ec != std::errc{}) {
        return content_header_detail::fail(error, "Content version does not fit in the header.");
    }

    const std::size_t prefixSize = static_cast<std::size_t>(versionResult.ptr - header.data());
    if (type.size() > header.size() - prefixSize) {
        return content_header_detail::fail(error, "Content type does not fit in the 10-byte header.");
    }
    std::copy(type.begin(), type.end(), header.begin() + static_cast<std::ptrdiff_t>(prefixSize));

    outHeader = header;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

// Text assets use visible '-' padding so the complete file remains friendly
// to text editors. The token still occupies the same fixed ten-byte header.
inline bool encodeTextContentFileHeader(
    std::uint32_t version,
    std::string_view type,
    std::array<char, kContentFileHeaderSize>& outHeader,
    std::string* error = nullptr
) {
    std::array<char, kContentFileHeaderSize> binaryHeader{};
    if (!encodeContentFileHeader(version, type, binaryHeader, error)) {
        return false;
    }

    const auto paddingStart = std::find(binaryHeader.begin(), binaryHeader.end(), '\0');
    if (paddingStart == binaryHeader.end()) {
        return content_header_detail::fail(
            error,
            "Text content header needs at least one byte of visible padding."
        );
    }
    std::fill(paddingStart, binaryHeader.end(), '-');
    outHeader = binaryHeader;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

inline bool decodeContentFileHeader(
    std::string_view bytes,
    ContentFileHeader& outHeader,
    std::string* error = nullptr
) {
    if (bytes.size() < kContentFileHeaderSize) {
        return content_header_detail::fail(error, "Content file is shorter than its 10-byte header.");
    }

    const std::string_view headerBytes = bytes.substr(0u, kContentFileHeaderSize);
    const std::size_t zeroPaddingOffset = headerBytes.find('\0');
    const std::size_t textPaddingOffset = headerBytes.find('-');
    std::size_t tokenSize = headerBytes.size();
    if (zeroPaddingOffset != std::string_view::npos &&
        textPaddingOffset != std::string_view::npos) {
        return content_header_detail::fail(
            error,
            "Content header mixes binary and text padding."
        );
    }
    if (zeroPaddingOffset != std::string_view::npos) {
        tokenSize = zeroPaddingOffset;
        for (std::size_t index = tokenSize; index < headerBytes.size(); ++index) {
            if (headerBytes[index] != '\0') {
                return content_header_detail::fail(
                    error,
                    "Content header contains non-zero bytes after its token."
                );
            }
        }
    } else if (textPaddingOffset != std::string_view::npos) {
        tokenSize = textPaddingOffset;
        for (std::size_t index = tokenSize; index < headerBytes.size(); ++index) {
            if (headerBytes[index] != '-') {
                return content_header_detail::fail(
                    error,
                    "Text content header contains invalid visible padding."
                );
            }
        }
    }

    if (tokenSize < 3u || headerBytes[0] != 'V') {
        return content_header_detail::fail(error, "Content header must start with V<version><type>.");
    }

    std::size_t typeOffset = 1u;
    while (typeOffset < tokenSize &&
           headerBytes[typeOffset] >= '0' && headerBytes[typeOffset] <= '9') {
        ++typeOffset;
    }
    if (typeOffset == 1u || typeOffset == tokenSize) {
        return content_header_detail::fail(error, "Content header has no version or type.");
    }

    std::uint32_t version = 0u;
    const auto versionResult = std::from_chars(
        headerBytes.data() + 1,
        headerBytes.data() + static_cast<std::ptrdiff_t>(typeOffset),
        version
    );
    if (versionResult.ec != std::errc{} || version == 0u) {
        return content_header_detail::fail(error, "Content header has an invalid version.");
    }

    const std::string_view type = headerBytes.substr(typeOffset, tokenSize - typeOffset);
    if (!content_header_detail::validType(type)) {
        return content_header_detail::fail(
            error,
            "Content header type must contain only uppercase ASCII letters."
        );
    }

    outHeader = ContentFileHeader{version, std::string(type)};
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace core
