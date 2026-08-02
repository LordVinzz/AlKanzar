#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::perfetto_detail {

enum class ProtoWireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

enum class CounterUnit : std::uint64_t {
    Unspecified = 0,
    TimeNs = 1,
    SizeBytes = 3,
};

class ProtoWriter {
public:
    void addVarintField(std::uint32_t fieldNumber, std::uint64_t value);
    void addInt64Field(std::uint32_t fieldNumber, std::int64_t value);
    void addStringField(std::uint32_t fieldNumber, std::string_view value);
    void addMessageField(std::uint32_t fieldNumber, const std::string& value);
    [[nodiscard]] std::string finish() &&;

private:
    void addTag(std::uint32_t fieldNumber, ProtoWireType wireType);
    void addVarint(std::uint64_t value);

    std::string buffer_{};
};

struct TrackEventSpec {
    std::uint64_t timestampNs{0};
    std::uint64_t trackUuid{0};
    std::uint32_t type{0};
    std::string name{};
    bool hasIntCounter{false};
    std::int64_t intCounterValue{0};
};

struct TrackDescriptorSpec {
    std::uint64_t uuid{0};
    std::uint64_t parentUuid{0};
    std::string name{};
    bool isCounter{false};
    CounterUnit counterUnit{CounterUnit::TimeNs};
};

struct ScopeEdgeEvent {
    std::uint64_t timestampNs{0};
    std::uint64_t startNs{0};
    std::uint64_t endNs{0};
    std::string name{};
    bool isBegin{false};
};

struct ResourceTrackKey {
    std::string category{};
    std::string name{};
    bool gpu{false};

    bool operator<(const ResourceTrackKey& other) const;
};

struct MetricTrackKey {
    std::string group{};
    std::string name{};

    bool operator<(const MetricTrackKey& other) const;
};

[[nodiscard]] std::string threadLabel(std::uint64_t threadId, std::uint64_t mainThreadId);
[[nodiscard]] std::string resourceTrackName(const ResourceTrackKey& key);
[[nodiscard]] std::int64_t msToNs(double durationMs);
[[nodiscard]] std::string encodeTrackDescriptor(const TrackDescriptorSpec& descriptor);
[[nodiscard]] std::string encodeTrackEvent(const TrackEventSpec& event);
[[nodiscard]] std::string encodeTracePacket(
    const std::string& payload,
    std::uint32_t payloadFieldNumber,
    std::uint32_t sequenceId,
    std::uint64_t timestampNs,
    bool hasTimestamp,
    bool clearIncrementalState
);
void addPacketToTrace(ProtoWriter& traceWriter, const std::string& packet);
[[nodiscard]] std::uint64_t relativeTimestamp(std::uint64_t absoluteNs, std::uint64_t traceStartNs);
void appendSlicePackets(
    ProtoWriter& traceWriter,
    const std::vector<ScopeEdgeEvent>& edges,
    std::uint64_t trackUuid,
    std::uint32_t sequenceId
);
void appendCounterPacket(
    ProtoWriter& traceWriter,
    std::uint32_t sequenceId,
    std::uint64_t timestampNs,
    std::uint64_t trackUuid,
    std::int64_t value
);

}  // namespace core::perfetto_detail
