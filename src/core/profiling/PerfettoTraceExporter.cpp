#include "ProfilerService.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

namespace {

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
    void addVarintField(std::uint32_t fieldNumber, std::uint64_t value) {
        addTag(fieldNumber, ProtoWireType::Varint);
        addVarint(value);
    }

    void addInt64Field(std::uint32_t fieldNumber, std::int64_t value) {
        addVarintField(fieldNumber, static_cast<std::uint64_t>(value));
    }

    void addStringField(std::uint32_t fieldNumber, std::string_view value) {
        addTag(fieldNumber, ProtoWireType::LengthDelimited);
        addVarint(static_cast<std::uint64_t>(value.size()));
        buffer_.append(value.data(), value.size());
    }

    void addMessageField(std::uint32_t fieldNumber, const std::string& value) {
        addTag(fieldNumber, ProtoWireType::LengthDelimited);
        addVarint(static_cast<std::uint64_t>(value.size()));
        buffer_.append(value);
    }

    [[nodiscard]] std::string finish() && { return std::move(buffer_); }

private:
    void addTag(std::uint32_t fieldNumber, ProtoWireType wireType) {
        addVarint((static_cast<std::uint64_t>(fieldNumber) << 3u) | static_cast<std::uint8_t>(wireType));
    }

    void addVarint(std::uint64_t value) {
        while (value >= 0x80u) {
            buffer_.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
            value >>= 7u;
        }
        buffer_.push_back(static_cast<char>(value));
    }

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

    bool operator<(const ResourceTrackKey& other) const {
        if (category != other.category) {
            return category < other.category;
        }
        if (name != other.name) {
            return name < other.name;
        }
        return gpu < other.gpu;
    }
};

struct MetricTrackKey {
    std::string group{};
    std::string name{};

    bool operator<(const MetricTrackKey& other) const {
        if (group != other.group) {
            return group < other.group;
        }
        return name < other.name;
    }
};

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

}  // namespace

bool exportProfilerTraceCaptureToPerfetto(
    const ProfilerTraceCapture& capture,
    const std::string& path,
    std::string* error
) {
    if (error != nullptr) {
        error->clear();
    }

    if (capture.frames.empty()) {
        if (error != nullptr) {
            *error = "No captured profiler frames are available for export.";
        }
        return false;
    }

    std::uint64_t traceStartNs = 0u;
    bool foundStart = false;
    for (const ProfilerTraceFrame& frame : capture.frames) {
        if (frame.endNs < frame.startNs) {
            continue;
        }
        if (!foundStart || frame.startNs < traceStartNs) {
            traceStartNs = frame.startNs;
            foundStart = true;
        }
    }
    if (!foundStart) {
        if (error != nullptr) {
            *error = "Profiler capture contains no complete frames.";
        }
        return false;
    }

    constexpr std::uint32_t kSequenceId = 1u;
    std::uint64_t nextTrackUuid = 1u;

    const std::uint64_t frameTrackUuid = nextTrackUuid++;
    const std::uint64_t cpuRootTrackUuid = nextTrackUuid++;
    const std::uint64_t gpuRootTrackUuid = nextTrackUuid++;
    const std::uint64_t resourceRootTrackUuid = nextTrackUuid++;
    const std::uint64_t metricsRootTrackUuid = nextTrackUuid++;

    std::map<std::uint64_t, std::uint64_t> threadTrackUuids{};
    std::map<std::string, std::uint64_t> gpuPassTrackUuids{};
    std::map<ResourceTrackKey, std::uint64_t> resourceTrackUuids{};
    std::map<std::string, std::uint64_t> metricGroupTrackUuids{};
    std::map<MetricTrackKey, std::uint64_t> metricTrackUuids{};

    for (const ProfilerTraceFrame& frame : capture.frames) {
        for (const ProfilerRecordedScope& scope : frame.cpuScopes) {
            threadTrackUuids.try_emplace(scope.threadId, 0u);
        }
        for (const GpuPassSample& pass : frame.gpuPasses) {
            gpuPassTrackUuids.try_emplace(pass.name, 0u);
        }
        for (const ResourceMemoryEntry& resource : frame.resources) {
            if (resource.cpuBytes > 0u) {
                resourceTrackUuids.try_emplace(ResourceTrackKey{resource.category, resource.name, false}, 0u);
            }
            if (resource.gpuBytes > 0u) {
                resourceTrackUuids.try_emplace(ResourceTrackKey{resource.category, resource.name, true}, 0u);
            }
        }
        for (const render::FrameCounterRecord& counter : frame.counters) {
            if (!counter.group.empty()) {
                metricGroupTrackUuids.try_emplace(counter.group, 0u);
            }
            metricTrackUuids.try_emplace(MetricTrackKey{counter.group, counter.name}, 0u);
        }
    }

    for (auto& [threadId, uuid] : threadTrackUuids) {
        (void)threadId;
        uuid = nextTrackUuid++;
    }

    const std::uint64_t gpuFrameTrackUuid = nextTrackUuid++;
    for (auto& [name, uuid] : gpuPassTrackUuids) {
        (void)name;
        uuid = nextTrackUuid++;
    }

    for (auto& [key, uuid] : resourceTrackUuids) {
        (void)key;
        uuid = nextTrackUuid++;
    }

    for (auto& [group, uuid] : metricGroupTrackUuids) {
        (void)group;
        uuid = nextTrackUuid++;
    }

    for (auto& [key, uuid] : metricTrackUuids) {
        (void)key;
        uuid = nextTrackUuid++;
    }

    ProtoWriter traceWriter;

    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{frameTrackUuid, 0u, "Frames", false, CounterUnit::TimeNs}),
            60,
            kSequenceId,
            0u,
            false,
            true
        )
    );
    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{cpuRootTrackUuid, 0u, "CPU", false, CounterUnit::TimeNs}),
            60,
            kSequenceId,
            0u,
            false,
            false
        )
    );
    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{gpuRootTrackUuid, 0u, "GPU", false, CounterUnit::TimeNs}),
            60,
            kSequenceId,
            0u,
            false,
            false
        )
    );
    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{resourceRootTrackUuid, 0u, "Resources", false, CounterUnit::TimeNs}),
            60,
            kSequenceId,
            0u,
            false,
            false
        )
    );
    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{metricsRootTrackUuid, 0u, "Metrics", false, CounterUnit::TimeNs}),
            60,
            kSequenceId,
            0u,
            false,
            false
        )
    );

    for (const auto& [threadId, uuid] : threadTrackUuids) {
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(
                encodeTrackDescriptor(TrackDescriptorSpec{
                    uuid,
                    cpuRootTrackUuid,
                    threadLabel(threadId, capture.mainThreadId),
                    false,
                    CounterUnit::TimeNs
                }),
                60,
                kSequenceId,
                0u,
                false,
                false
            )
        );
    }

    addPacketToTrace(
        traceWriter,
        encodeTracePacket(
            encodeTrackDescriptor(TrackDescriptorSpec{
                gpuFrameTrackUuid,
                gpuRootTrackUuid,
                "GPU Timed Passes",
                true,
                CounterUnit::TimeNs
            }),
            60,
            kSequenceId,
            0u,
            false,
            false
        )
    );

    for (const auto& [name, uuid] : gpuPassTrackUuids) {
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(
                encodeTrackDescriptor(TrackDescriptorSpec{uuid, gpuRootTrackUuid, name, true, CounterUnit::TimeNs}),
                60,
                kSequenceId,
                0u,
                false,
                false
            )
        );
    }

    for (const auto& [key, uuid] : resourceTrackUuids) {
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(
                encodeTrackDescriptor(TrackDescriptorSpec{
                    uuid,
                    resourceRootTrackUuid,
                    resourceTrackName(key),
                    true,
                    CounterUnit::SizeBytes
                }),
                60,
                kSequenceId,
                0u,
                false,
                false
            )
        );
    }

    for (const auto& [group, uuid] : metricGroupTrackUuids) {
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(
                encodeTrackDescriptor(TrackDescriptorSpec{
                    uuid,
                    metricsRootTrackUuid,
                    group,
                    false,
                    CounterUnit::Unspecified
                }),
                60,
                kSequenceId,
                0u,
                false,
                false
            )
        );
    }

    for (const auto& [key, uuid] : metricTrackUuids) {
        addPacketToTrace(
            traceWriter,
            encodeTracePacket(
                encodeTrackDescriptor(TrackDescriptorSpec{
                    uuid,
                    key.group.empty() ? metricsRootTrackUuid : metricGroupTrackUuids.at(key.group),
                    key.name,
                    true,
                    CounterUnit::Unspecified
                }),
                60,
                kSequenceId,
                0u,
                false,
                false
            )
        );
    }

    for (const ProfilerTraceFrame& frame : capture.frames) {
        const std::uint64_t frameStartNs = relativeTimestamp(frame.startNs, traceStartNs);
        const std::uint64_t frameEndNs = relativeTimestamp(frame.endNs, traceStartNs);
        if (frameEndNs < frameStartNs) {
            continue;
        }

        const std::string frameName = "Frame #" + std::to_string(frame.frameNumber);
        appendSlicePackets(
            traceWriter,
            std::vector<ScopeEdgeEvent>{
                ScopeEdgeEvent{frameStartNs, frameStartNs, frameEndNs, frameName, true},
                ScopeEdgeEvent{frameEndNs, frameStartNs, frameEndNs, frameName, false},
            },
            frameTrackUuid,
            kSequenceId
        );

        std::unordered_map<std::uint64_t, std::vector<ScopeEdgeEvent>> edgesByThread{};
        for (const ProfilerRecordedScope& scope : frame.cpuScopes) {
            if (scope.endNs < scope.startNs) {
                continue;
            }

            const std::uint64_t startNs = relativeTimestamp(scope.startNs, traceStartNs);
            const std::uint64_t endNs = relativeTimestamp(scope.endNs, traceStartNs);
            auto& edges = edgesByThread[scope.threadId];
            edges.push_back(ScopeEdgeEvent{startNs, startNs, endNs, scope.name, true});
            edges.push_back(ScopeEdgeEvent{endNs, startNs, endNs, scope.name, false});
        }

        for (auto& [threadId, edges] : edgesByThread) {
            std::sort(edges.begin(), edges.end(), [](const ScopeEdgeEvent& lhs, const ScopeEdgeEvent& rhs) {
                if (lhs.timestampNs != rhs.timestampNs) {
                    return lhs.timestampNs < rhs.timestampNs;
                }
                if (lhs.isBegin != rhs.isBegin) {
                    return !lhs.isBegin && rhs.isBegin;
                }
                if (lhs.isBegin) {
                    if (lhs.endNs != rhs.endNs) {
                        return lhs.endNs > rhs.endNs;
                    }
                    return lhs.name < rhs.name;
                }
                if (lhs.startNs != rhs.startNs) {
                    return lhs.startNs > rhs.startNs;
                }
                return lhs.name < rhs.name;
            });
            appendSlicePackets(traceWriter, edges, threadTrackUuids.at(threadId), kSequenceId);
        }

        const std::uint64_t sampleTimestampNs = frameEndNs;
        const bool gpuComplete = !frame.gpuPasses.empty() &&
            std::all_of(frame.gpuPasses.begin(), frame.gpuPasses.end(), [](const GpuPassSample& pass) {
                return pass.available && !pass.pending;
            });

        if (gpuComplete) {
            std::int64_t totalGpuNs = 0;
            for (const GpuPassSample& pass : frame.gpuPasses) {
                const std::int64_t durationNs = msToNs(pass.durationMs);
                totalGpuNs += durationNs;
                appendCounterPacket(traceWriter, kSequenceId, sampleTimestampNs, gpuPassTrackUuids.at(pass.name), durationNs);
            }
            appendCounterPacket(traceWriter, kSequenceId, sampleTimestampNs, gpuFrameTrackUuid, totalGpuNs);
        } else {
            for (const GpuPassSample& pass : frame.gpuPasses) {
                if (pass.available && !pass.pending) {
                    appendCounterPacket(
                        traceWriter,
                        kSequenceId,
                        sampleTimestampNs,
                        gpuPassTrackUuids.at(pass.name),
                        msToNs(pass.durationMs)
                    );
                }
            }
        }

        for (const ResourceMemoryEntry& resource : frame.resources) {
            if (resource.cpuBytes > 0u) {
                appendCounterPacket(
                    traceWriter,
                    kSequenceId,
                    sampleTimestampNs,
                    resourceTrackUuids.at(ResourceTrackKey{resource.category, resource.name, false}),
                    static_cast<std::int64_t>(resource.cpuBytes)
                );
            }
            if (resource.gpuBytes > 0u) {
                appendCounterPacket(
                    traceWriter,
                    kSequenceId,
                    sampleTimestampNs,
                    resourceTrackUuids.at(ResourceTrackKey{resource.category, resource.name, true}),
                    static_cast<std::int64_t>(resource.gpuBytes)
                );
            }
        }

        for (const render::FrameCounterRecord& counter : frame.counters) {
            appendCounterPacket(
                traceWriter,
                kSequenceId,
                sampleTimestampNs,
                metricTrackUuids.at(MetricTrackKey{counter.group, counter.name}),
                counter.value
            );
        }
    }

    const std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::error_code createError{};
        std::filesystem::create_directories(outputPath.parent_path(), createError);
        if (createError) {
            if (error != nullptr) {
                *error = "Failed to create export directory: " + createError.message();
            }
            return false;
        }
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "Failed to open export file for writing.";
        }
        return false;
    }

    const std::string payload = std::move(traceWriter).finish();
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!output.good()) {
        if (error != nullptr) {
            *error = "Failed while writing the Perfetto trace file.";
        }
        return false;
    }

    return true;
}

}  // namespace core
