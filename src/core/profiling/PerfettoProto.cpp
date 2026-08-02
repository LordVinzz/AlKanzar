#include "PerfettoProto.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace core::perfetto_detail {

void ProtoWriter::addVarintField(std::uint32_t fieldNumber, std::uint64_t value) {
    addTag(fieldNumber, ProtoWireType::Varint);
    addVarint(value);
}

void ProtoWriter::addInt64Field(std::uint32_t fieldNumber, std::int64_t value) {
    addVarintField(fieldNumber, static_cast<std::uint64_t>(value));
}

void ProtoWriter::addStringField(std::uint32_t fieldNumber, std::string_view value) {
    addTag(fieldNumber, ProtoWireType::LengthDelimited);
    addVarint(static_cast<std::uint64_t>(value.size()));
    buffer_.append(value.data(), value.size());
}

void ProtoWriter::addMessageField(std::uint32_t fieldNumber, const std::string& value) {
    addTag(fieldNumber, ProtoWireType::LengthDelimited);
    addVarint(static_cast<std::uint64_t>(value.size()));
    buffer_.append(value);
}

std::string ProtoWriter::finish() && {
    return std::move(buffer_);
}

void ProtoWriter::addTag(std::uint32_t fieldNumber, ProtoWireType wireType) {
    addVarint((static_cast<std::uint64_t>(fieldNumber) << 3u) | static_cast<std::uint8_t>(wireType));
}

void ProtoWriter::addVarint(std::uint64_t value) {
    while (value >= 0x80u) {
        buffer_.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
        value >>= 7u;
    }
    buffer_.push_back(static_cast<char>(value));
}

bool ResourceTrackKey::operator<(const ResourceTrackKey& other) const {
    if (category != other.category) {
        return category < other.category;
    }
    if (name != other.name) {
        return name < other.name;
    }
    return gpu < other.gpu;
}

bool MetricTrackKey::operator<(const MetricTrackKey& other) const {
    return group != other.group ? group < other.group : name < other.name;
}

std::string threadLabel(std::uint64_t threadId, std::uint64_t mainThreadId) {
    if (threadId == mainThreadId) {
        return "Main";
    }

    std::ostringstream stream;
    stream << "T" << threadId;
    return stream.str();
}

std::string resourceTrackName(const ResourceTrackKey& key) {
    return key.category + ": " + key.name + (key.gpu ? " (GPU)" : " (RAM)");
}

std::int64_t msToNs(double durationMs) {
    return static_cast<std::int64_t>(std::llround(std::max(durationMs, 0.0) * 1'000'000.0));
}

std::string encodeCounterDescriptor(CounterUnit unit) {
    ProtoWriter writer;
    if (unit != CounterUnit::Unspecified) {
        writer.addVarintField(3, static_cast<std::uint64_t>(unit));
    }
    return std::move(writer).finish();
}

std::string encodeTrackDescriptor(const TrackDescriptorSpec& descriptor) {
    ProtoWriter writer;
    writer.addVarintField(1, descriptor.uuid);
    if (descriptor.parentUuid != 0u) {
        writer.addVarintField(5, descriptor.parentUuid);
    }
    if (!descriptor.name.empty()) {
        writer.addStringField(2, descriptor.name);
    }
    if (descriptor.isCounter) {
        writer.addMessageField(8, encodeCounterDescriptor(descriptor.counterUnit));
    }
    return std::move(writer).finish();
}

std::string encodeTrackEvent(const TrackEventSpec& event) {
    ProtoWriter writer;
    writer.addVarintField(9, event.type);
    writer.addVarintField(11, event.trackUuid);
    if (!event.name.empty()) {
        writer.addStringField(23, event.name);
    }
    if (event.hasIntCounter) {
        writer.addInt64Field(30, event.intCounterValue);
    }
    return std::move(writer).finish();
}

std::string encodeTracePacket(
    const std::string& payload,
    std::uint32_t payloadFieldNumber,
    std::uint32_t sequenceId,
    std::uint64_t timestampNs,
    bool hasTimestamp,
    bool clearIncrementalState
) {
    ProtoWriter writer;
    if (hasTimestamp) {
        writer.addVarintField(8, timestampNs);
    }
    writer.addVarintField(10, sequenceId);
    if (clearIncrementalState) {
        writer.addVarintField(13, 1u);
    }
    writer.addMessageField(payloadFieldNumber, payload);
    return std::move(writer).finish();
}

void addPacketToTrace(ProtoWriter& traceWriter, const std::string& packet) {
    traceWriter.addMessageField(1, packet);
}

std::uint64_t relativeTimestamp(std::uint64_t absoluteNs, std::uint64_t traceStartNs) {
    return absoluteNs >= traceStartNs ? absoluteNs - traceStartNs : 0u;
}

void appendSlicePackets(
    ProtoWriter& traceWriter,
    const std::vector<ScopeEdgeEvent>& edges,
    std::uint64_t trackUuid,
    std::uint32_t sequenceId
) {
    for (const ScopeEdgeEvent& edge : edges) {
        TrackEventSpec event{};
        event.timestampNs = edge.timestampNs;
        event.trackUuid = trackUuid;
        event.type = edge.isBegin ? 1u : 2u;
        if (edge.isBegin) {
            event.name = edge.name;
        }
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(encodeTrackEvent(event), 11, sequenceId, edge.timestampNs, true, false)
        );
    }
}

void appendCounterPacket(
    ProtoWriter& traceWriter,
    std::uint32_t sequenceId,
    std::uint64_t timestampNs,
    std::uint64_t trackUuid,
    std::int64_t value
) {
    TrackEventSpec event{};
    event.timestampNs = timestampNs;
    event.trackUuid = trackUuid;
    event.type = 4u;
    event.hasIntCounter = true;
    event.intCounterValue = value;
    addPacketToTrace(
        traceWriter,
        encodeTracePacket(encodeTrackEvent(event), 11, sequenceId, timestampNs, true, false)
    );
}

}  // namespace core::perfetto_detail

