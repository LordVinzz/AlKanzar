#include "core/editor/EditorUi.hpp"
#include "core/editor/EditorUiCommands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include <spdlog/spdlog.h>

#include "core/app/EngineServices.hpp"
#include "core/editor/EditorSessionImGuiSettings.hpp"
#include "core/ecs/ComponentRegistry.hpp"
#include "render/resources/StaticGltfModel.hpp"


namespace core {

std::string formatProfilerBytes(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < 3) {
        value /= 1024.0;
        ++unitIndex;
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, kUnits[unitIndex]);
    return buffer;
}

const char* profilerStatusLabel(const ProfilerStats& stats) {
    if (stats.startPending) {
        return "Starting next frame";
    }
    if (stats.stopPending) {
        return "Stopping after current frame";
    }
    if (stats.capturing) {
        return "Capturing";
    }
    return "Idle";
}

void drawProfilerScopeRows(const std::vector<ProfilerScopeNode>& nodes) {
    for (const ProfilerScopeNode& node : nodes) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const ImGuiTreeNodeFlags flags = node.children.empty()
            ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
            : ImGuiTreeNodeFlags_DefaultOpen;
        const bool open = ImGui::TreeNodeEx(static_cast<const void*>(&node), flags, "%s", node.name.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f", node.totalMs);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%.3f", node.selfMs);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", node.callCount);
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(node.thread.c_str());

        if (open && !node.children.empty()) {
            drawProfilerScopeRows(node.children);
            ImGui::TreePop();
        }
    }
}

void drawProfilerMemoryGroup(const char* label, const std::vector<const ResourceMemoryEntry*>& entries) {
    ImGui::SeparatorText(label);
    if (entries.empty()) {
        ImGui::TextUnformatted("No resources recorded in this residency group.");
        return;
    }

    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> totalsByCategory{};
    std::uint64_t totalCpuBytes = 0u;
    std::uint64_t totalGpuBytes = 0u;
    for (const ResourceMemoryEntry* entry : entries) {
        totalCpuBytes += entry->cpuBytes;
        totalGpuBytes += entry->gpuBytes;
        auto& totals = totalsByCategory[entry->category];
        totals.first += entry->cpuBytes;
        totals.second += entry->gpuBytes;
    }

    ImGui::Text(
        "Totals: %zu resources | RAM %s | GPU %s",
        entries.size(),
        formatProfilerBytes(totalCpuBytes).c_str(),
        formatProfilerBytes(totalGpuBytes).c_str()
    );

    const std::string categoryTableId = std::string("ProfilerCategoryTotals##") + label;
    if (ImGui::BeginTable(categoryTableId.c_str(), 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("RAM");
        ImGui::TableSetupColumn("GPU");
        ImGui::TableHeadersRow();
        for (const auto& [category, totals] : totalsByCategory) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(category.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(formatProfilerBytes(totals.first).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(formatProfilerBytes(totals.second).c_str());
        }
        ImGui::EndTable();
    }

    const std::string resourceTableId = std::string("ProfilerResources##") + label;
    if (ImGui::BeginTable(
            resourceTableId.c_str(),
            4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Resource");
        ImGui::TableSetupColumn("RAM");
        ImGui::TableSetupColumn("GPU");
        ImGui::TableHeadersRow();
        for (const ResourceMemoryEntry* entry : entries) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(entry->category.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(entry->name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(entry->cpuBytes > 0u ? formatProfilerBytes(entry->cpuBytes).c_str() : "--");
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(entry->gpuBytes > 0u ? formatProfilerBytes(entry->gpuBytes).c_str() : "--");
        }
        ImGui::EndTable();
    }
}

double profilerCadenceMs(
    const std::vector<std::shared_ptr<const ProfilerFrameSnapshot>>& snapshots,
    int frameIndex
) {
    if (frameIndex < 0 || frameIndex >= static_cast<int>(snapshots.size())) {
        return 0.0;
    }

    const auto cadenceBetween = [](const std::shared_ptr<const ProfilerFrameSnapshot>& earlier,
                                   const std::shared_ptr<const ProfilerFrameSnapshot>& later) {
        if (!earlier || !later || earlier->sessionId != later->sessionId || later->startNs <= earlier->startNs) {
            return 0.0;
        }
        return static_cast<double>(later->startNs - earlier->startNs) / 1'000'000.0;
    };

    if (frameIndex > 0) {
        const double previousCadenceMs = cadenceBetween(snapshots[frameIndex - 1], snapshots[frameIndex]);
        if (previousCadenceMs > 0.0) {
            return previousCadenceMs;
        }
    }
    if (frameIndex + 1 < static_cast<int>(snapshots.size())) {
        return cadenceBetween(snapshots[frameIndex], snapshots[frameIndex + 1]);
    }
    return 0.0;
}

std::string buildProfilerWindowTitle(
    const std::vector<std::shared_ptr<const ProfilerFrameSnapshot>>& snapshots,
    int frameIndex
) {
    const double cadenceMs = profilerCadenceMs(snapshots, frameIndex);
    if (cadenceMs <= 0.0) {
        return "Profiler###ProfilerWindow";
    }

    const double fps = 1000.0 / cadenceMs;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "Profiler (%.1f FPS)###ProfilerWindow", fps);
    return buffer;
}

void drawProfilerWindow(EngineServices& services) {
    if (!services.editorSession.profilerWindowVisible) {
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    const auto snapshots = services.profiler.snapshots();
    int titleFrameIndex = -1;
    if (!snapshots.empty()) {
        titleFrameIndex = services.editorSession.profilerSelectedFrame;
        if (services.editorSession.profilerFollowLatest ||
            titleFrameIndex < 0 ||
            titleFrameIndex >= static_cast<int>(snapshots.size())) {
            titleFrameIndex = static_cast<int>(snapshots.size()) - 1;
        }
    }

    if (services.editorSession.profilerWindowFocusRequested) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(860.0f, 720.0f), ImGuiCond_FirstUseEver);
    const bool wasOpen = services.editorSession.profilerWindowVisible;
    bool open = wasOpen;
    const std::string profilerWindowTitle = buildProfilerWindowTitle(snapshots, titleFrameIndex);
    if (!ImGui::Begin(profilerWindowTitle.c_str(), &open)) {
        ImGui::End();
        services.editorSession.profilerWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    const ProfilerStats stats = services.profiler.stats();

    ImGui::BeginDisabled(stats.capturing || stats.startPending);
    if (ImGui::Button("Start Profiling")) {
        services.profiler.startCapture();
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, true);
        services.editorSession.profilerExportStatus.clear();
        services.editorSession.profilerExportStatusIsError = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled((!stats.capturing && !stats.stopPending) || stats.startPending);
    if (ImGui::Button("Stop Profiling")) {
        services.profiler.stopCapture();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(stats.bufferedFrames == 0u);
    if (ImGui::Button("Export Perfetto Trace")) {
        const ProfilerTraceCapture capture = services.profiler.rawCapture();
        const std::filesystem::path exportPath = defaultProfilerExportPath(capture);
        std::string exportError{};
        if (services.profiler.exportPerfettoTrace(exportPath.string(), &exportError)) {
            services.editorSession.profilerExportStatus =
                "Exported Perfetto trace to " + std::filesystem::absolute(exportPath).string();
            services.editorSession.profilerExportStatusIsError = false;
        } else {
            services.editorSession.profilerExportStatus = exportError.empty()
                ? "Perfetto export failed."
                : exportError;
            services.editorSession.profilerExportStatusIsError = true;
        }
    }
    ImGui::EndDisabled();

    if (!services.editorSession.profilerExportStatus.empty()) {
        if (services.editorSession.profilerExportStatusIsError) {
            ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "%s", services.editorSession.profilerExportStatus.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 0.40f, 1.0f), "%s", services.editorSession.profilerExportStatus.c_str());
        }
    }

    ImGui::Text(
        "Status: %s | Buffered: %zu | Pending GPU Frames: %zu",
        profilerStatusLabel(stats),
        stats.bufferedFrames,
        stats.pendingGpuFrames
    );
    ImGui::Text(
        "Dropped CPU scopes: %llu | Dropped GPU scopes: %llu | Dropped frames: %llu",
        static_cast<unsigned long long>(stats.droppedCpuScopes),
        static_cast<unsigned long long>(stats.droppedGpuScopes),
        static_cast<unsigned long long>(stats.droppedFrames)
    );

    if (snapshots.empty()) {
        ImGui::SeparatorText("Frames");
        ImGui::TextUnformatted("No captured frames available yet.");
        ImGui::End();
        services.editorSession.profilerWindowVisible = open;
        if (wasOpen != open) {
            markEditorSessionImGuiSettingsDirty();
        }
        services.editorSession.profilerWindowFocusRequested = false;
        return;
    }

    if (services.editorSession.profilerFollowLatest ||
        services.editorSession.profilerSelectedFrame < 0 ||
        services.editorSession.profilerSelectedFrame >= static_cast<int>(snapshots.size())) {
        services.editorSession.profilerSelectedFrame = static_cast<int>(snapshots.size()) - 1;
    }

    ImGui::SeparatorText("Frame History");
    bool followLatest = services.editorSession.profilerFollowLatest;
    if (ImGui::Checkbox("Follow Latest", &followLatest)) {
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, followLatest);
        if (followLatest) {
            services.editorSession.profilerSelectedFrame = static_cast<int>(snapshots.size()) - 1;
        }
    }

    std::vector<float> cpuTotals{};
    std::vector<float> gpuTotals{};
    cpuTotals.reserve(snapshots.size());
    gpuTotals.reserve(snapshots.size());
    float maxGraphValue = 1.0f;
    for (const auto& snapshot : snapshots) {
        cpuTotals.push_back(static_cast<float>(snapshot->cpuFrameMs));
        gpuTotals.push_back(static_cast<float>(snapshot->gpuFrameMs));
        maxGraphValue = std::max(maxGraphValue, std::max(cpuTotals.back(), gpuTotals.back()));
    }

    ImGui::PlotLines("CPU Frame (ms)", cpuTotals.data(), static_cast<int>(cpuTotals.size()), 0, nullptr, 0.0f, maxGraphValue, ImVec2(-1.0f, 80.0f));
    ImGui::PlotLines(
        "GPU Timed Passes (ms)",
        gpuTotals.data(),
        static_cast<int>(gpuTotals.size()),
        0,
        nullptr,
        0.0f,
        maxGraphValue,
        ImVec2(-1.0f, 80.0f)
    );

    int selectedFrame = services.editorSession.profilerSelectedFrame;
    if (ImGui::SliderInt("Selected Frame", &selectedFrame, 0, static_cast<int>(snapshots.size()) - 1)) {
        services.editorSession.profilerSelectedFrame = selectedFrame;
        setPersistedEditorSessionFlag(services.editorSession.profilerFollowLatest, false);
    }

    const std::shared_ptr<const ProfilerFrameSnapshot>& selected = snapshots[services.editorSession.profilerSelectedFrame];
    ImGui::Text(
        "Frame #%llu | CPU %.3f ms | Profiler UI %.3f ms",
        static_cast<unsigned long long>(selected->frameNumber),
        selected->cpuFrameMs,
        selected->profilerUiMs
    );
    if (selected->gpuComplete) {
        ImGui::SameLine();
        ImGui::Text("GPU Timed Total %.3f ms", selected->gpuFrameMs);
    } else {
        ImGui::SameLine();
        ImGui::TextUnformatted("GPU Pending/Unavailable");
    }

    std::vector<const render::FrameCounterRecord*> frustumCounters{};
    frustumCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> occlusionCounters{};
    occlusionCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> runtimePolicyCounters{};
    runtimePolicyCounters.reserve(selected->counters.size());
    std::vector<const render::FrameCounterRecord*> generalCounters{};
    generalCounters.reserve(selected->counters.size());

    const auto counterValue = [&](std::string_view group, std::string_view name) -> std::optional<std::int64_t> {
        const auto it = std::find_if(selected->counters.begin(), selected->counters.end(), [group, name](const render::FrameCounterRecord& counter) {
            return counter.group == group && counter.name == name;
        });
        if (it == selected->counters.end()) {
            return std::nullopt;
        }
        return it->value;
    };
    for (const render::FrameCounterRecord& counter : selected->counters) {
        if (counter.group == "Frustum Culling") {
            frustumCounters.push_back(&counter);
        } else if (counter.group == "Occlusion Culling") {
            occlusionCounters.push_back(&counter);
        } else if (counter.group == "Runtime Policy") {
            runtimePolicyCounters.push_back(&counter);
        } else {
            generalCounters.push_back(&counter);
        }
    }

    sortProfilerCounters(frustumCounters);
    sortProfilerCounters(occlusionCounters);
    sortProfilerCounters(runtimePolicyCounters);
    sortProfilerCounters(generalCounters);

    if (!frustumCounters.empty()) {
        ImGui::SeparatorText("Frustum Culling");
        drawProfilerCounterTable("ProfilerFrustumCounters", frustumCounters);

        const double boundsTested = static_cast<double>(counterValue("Frustum Culling", "Bounds Tested").value_or(0));
        const double culled = static_cast<double>(counterValue("Frustum Culling", "Culled").value_or(0));
        const double cullRatio = boundsTested > 0.0 ? culled / boundsTested : 0.0;
        ImGui::Text("Cull Ratio: %.2f%%", cullRatio * 100.0);
    }

    if (!occlusionCounters.empty()) {
        ImGui::SeparatorText("Occlusion Culling");
        drawProfilerCounterTable("ProfilerOcclusionCounters", occlusionCounters);

        const double candidates = static_cast<double>(counterValue("Occlusion Culling", "Candidates").value_or(0));
        const double occluded = static_cast<double>(counterValue("Occlusion Culling", "Occluded").value_or(0));
        const double occlusionRatio = candidates > 0.0 ? occluded / candidates : 0.0;
        ImGui::Text("Occlusion Ratio: %.2f%%", occlusionRatio * 100.0);
    }
    if (!runtimePolicyCounters.empty()) {
        ImGui::SeparatorText("Runtime Policy");
        drawProfilerCounterTable("ProfilerRuntimePolicyCounters", runtimePolicyCounters);
    }
    if (!generalCounters.empty()) {
        ImGui::SeparatorText("Metrics");
        drawProfilerCounterTable("ProfilerGeneralCounters", generalCounters);
    }

    ImGui::SeparatorText("CPU Call Tree");
    if (ImGui::BeginTable(
            "ProfilerCpuTree",
            5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Scope");
        ImGui::TableSetupColumn("Total ms");
        ImGui::TableSetupColumn("Self ms");
        ImGui::TableSetupColumn("Calls");
        ImGui::TableSetupColumn("Thread");
        ImGui::TableHeadersRow();
        drawProfilerScopeRows(selected->cpuScopes);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("GPU Timed Passes");
    if (ImGui::BeginTable(
            "ProfilerGpuPasses",
            3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Duration");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        for (const GpuPassSample& pass : selected->gpuPasses) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (pass.available) {
                ImGui::Text("%.3f ms", pass.durationMs);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::TableSetColumnIndex(2);
            if (pass.pending) {
                ImGui::TextUnformatted("Pending");
            } else if (pass.available) {
                ImGui::TextUnformatted("Ready");
            } else {
                ImGui::TextUnformatted("Unavailable");
            }
        }
        ImGui::EndTable();
    }

    std::vector<const ResourceMemoryEntry*> ramEntries{};
    std::vector<const ResourceMemoryEntry*> gpuEntries{};
    std::vector<const ResourceMemoryEntry*> bothEntries{};
    for (const ResourceMemoryEntry& entry : selected->resources) {
        if (entry.cpuBytes > 0u && entry.gpuBytes > 0u) {
            bothEntries.push_back(&entry);
        } else if (entry.cpuBytes > 0u) {
            ramEntries.push_back(&entry);
        } else if (entry.gpuBytes > 0u) {
            gpuEntries.push_back(&entry);
        }
    }

    drawProfilerMemoryGroup("Both", bothEntries);
    drawProfilerMemoryGroup("RAM", ramEntries);
    drawProfilerMemoryGroup("GPU", gpuEntries);

    ImGui::End();
    services.editorSession.profilerWindowVisible = open;
    if (wasOpen != open) {
        markEditorSessionImGuiSettingsDirty();
    }
    services.editorSession.profilerWindowFocusRequested = false;
}

}  // namespace core
