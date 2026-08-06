#include <array>
#include <cassert>
#include <string>
#include <string_view>

#include "core/content/ContentFileHeader.hpp"

namespace {

void testNavHeaderUsesExactlyTenBytes() {
    std::array<char, core::kContentFileHeaderSize> encoded{};
    std::string error{};
    assert(core::encodeContentFileHeader(1u, "NAV", encoded, &error));
    assert(error.empty());
    assert((encoded == std::array<char, core::kContentFileHeaderSize>{
        'V', '1', 'N', 'A', 'V', '\0', '\0', '\0', '\0', '\0'
    }));

    core::ContentFileHeader decoded{};
    assert(core::decodeContentFileHeader(
        std::string_view(encoded.data(), encoded.size()),
        decoded,
        &error
    ));
    assert(error.empty());
    assert((decoded == core::ContentFileHeader{1u, "NAV"}));
}

void testMultiDigitVersionRoundTrips() {
    std::array<char, core::kContentFileHeaderSize> encoded{};
    std::string error{};
    assert(core::encodeContentFileHeader(123u, "NAV", encoded, &error));

    core::ContentFileHeader decoded{};
    assert(core::decodeContentFileHeader(
        std::string_view(encoded.data(), encoded.size()),
        decoded,
        &error
    ));
    assert((decoded == core::ContentFileHeader{123u, "NAV"}));
}

void testTextHeaderUsesVisiblePadding() {
    std::array<char, core::kContentFileHeaderSize> encoded{};
    std::string error{};
    assert(core::encodeTextContentFileHeader(1u, "SCN", encoded, &error));
    assert(error.empty());
    assert(std::string(encoded.data(), encoded.size()) == "V1SCN-----");

    core::ContentFileHeader decoded{};
    assert(core::decodeContentFileHeader(
        std::string_view(encoded.data(), encoded.size()),
        decoded,
        &error
    ));
    assert((decoded == core::ContentFileHeader{1u, "SCN"}));
}

void testInvalidHeadersReturnActionableErrors() {
    std::array<char, core::kContentFileHeaderSize> encoded{};
    std::string error{};
    assert(!core::encodeContentFileHeader(0u, "NAV", encoded, &error));
    assert(!error.empty());
    assert(!core::encodeContentFileHeader(1u, "nav", encoded, &error));
    assert(!error.empty());
    assert(!core::encodeContentFileHeader(123456789u, "NAV", encoded, &error));
    assert(!error.empty());

    core::ContentFileHeader decoded{};
    assert(!core::decodeContentFileHeader("V1NAV", decoded, &error));
    assert(!error.empty());

    const std::string dirtyPadding{"V1NAV\0X\0\0\0", core::kContentFileHeaderSize};
    assert(!core::decodeContentFileHeader(dirtyPadding, decoded, &error));
    assert(!error.empty());

    const std::string dirtyTextPadding = "V1SCN----X";
    assert(!core::decodeContentFileHeader(dirtyTextPadding, decoded, &error));
    assert(!error.empty());

    const std::string mixedPadding{"V1SCN-\0---", core::kContentFileHeaderSize};
    assert(!core::decodeContentFileHeader(mixedPadding, decoded, &error));
    assert(!error.empty());

    const std::string missingType{"V123\0\0\0\0\0\0", core::kContentFileHeaderSize};
    assert(!core::decodeContentFileHeader(missingType, decoded, &error));
    assert(!error.empty());
}

}  // namespace

int main() {
    testNavHeaderUsesExactlyTenBytes();
    testMultiDigitVersionRoundTrips();
    testTextHeaderUsesVisiblePadding();
    testInvalidHeadersReturnActionableErrors();
    return 0;
}
