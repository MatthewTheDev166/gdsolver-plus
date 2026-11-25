#pragma once

#include "mod/plan_edges.hpp"
#include "mod/gdr/gdr.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <system_error>

namespace gdsolver::macro {

struct PlanStats {
    int clicks = 0;
    int releases = 0;
    int lastTick = 0;
    int minGapTicks = -1;
    double avgCps = 0.0;
    int peakCps = 0;
};

inline PlanStats computePlanStats(const std::vector<p1::InputCmd>& plan) {
    PlanStats st;
    if (plan.empty()) return st;

    std::vector<int> clickTicks;
    clickTicks.reserve(plan.size());
    int prevTick[2] = {-1, -1};

    for (const auto& cmd : plan) {
        if (cmd.step > st.lastTick) st.lastTick = cmd.step;
        const int slot = cmd.p2 ? 1 : 0;
        if (prevTick[slot] >= 0) {
            const int gap = cmd.step - prevTick[slot];
            if (gap >= 0 && (st.minGapTicks < 0 || gap < st.minGapTicks)) {
                st.minGapTicks = gap;
            }
        }
        prevTick[slot] = cmd.step;
        if (cmd.down) {
            ++st.clicks;
            clickTicks.push_back(cmd.step);
        } else {
            ++st.releases;
        }
    }

    if (!clickTicks.empty()) {
        std::sort(clickTicks.begin(), clickTicks.end());
        const int spanTicks = std::max(1, clickTicks.back() - clickTicks.front());
        st.avgCps = (clickTicks.size() > 1)
            ? (static_cast<double>(clickTicks.size() - 1) * 240.0 / static_cast<double>(spanTicks))
            : 0.0;

        size_t head = 0;
        for (size_t tail = 0; tail < clickTicks.size(); ++tail) {
            while (head < tail && clickTicks[tail] - clickTicks[head] >= 240) {
                ++head;
            }
            const int win = static_cast<int>(tail - head + 1);
            if (win > st.peakCps) st.peakCps = win;
        }
    }
    return st;
}

inline std::string formatPlanStats(const PlanStats& st) {
    if (st.clicks <= 0) return "No clicks in plan";
    char buf[128];
    if (st.minGapTicks >= 0) {
        std::snprintf(buf, sizeof(buf), "Clicks: %d | Avg %.1f / Peak %d CPS | Min gap: %dt",
                      st.clicks, st.avgCps, st.peakCps, st.minGapTicks);
    } else {
        std::snprintf(buf, sizeof(buf), "Clicks: %d | Peak %d CPS", st.clicks, st.peakCps);
    }
    return std::string(buf);
}

inline std::string sanitizeFilename(const std::string& name) {
    std::string safe;
    safe.reserve(name.size());
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 32 ||
            c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            safe += '_';
        } else {
            safe += c;
        }
    }
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) {
        safe.pop_back();
    }
    return safe;
}

inline bool exportGdr2(const std::filesystem::path& exportDir,
                       int levelId,
                       const std::string& levelName,
                       int coins,
                       const std::vector<p1::InputCmd>& plan,
                       std::string& outFilename,
                       std::string& outError,
                       int frameOffset = 0,
                       bool withCoins = false,
                       int totalTicks = 0) {
    if (plan.empty()) {
        outError = "Plan has no inputs";
        return false;
    }

    std::string safeName = sanitizeFilename(levelName);
    if (safeName.empty()) {
        safeName = "level_" + std::to_string(levelId);
    }
    outFilename = safeName + (withCoins ? "-coins-macro.gdr2" : "-macro.gdr2");

    std::error_code ec;
    std::filesystem::create_directories(exportDir, ec);
    if (ec) {
        outError = "Could not create directory: " + ec.message();
        return false;
    }

    std::filesystem::path fullPath = exportDir / outFilename;

    gdr::Replay<> replay("gdsolver", 700);
    replay.author = "gdsolver";
    replay.description = "Solved by gdsolver";
#ifdef GEODE_COMP_GD_VERSION
    replay.gameVersion = GEODE_COMP_GD_VERSION;
#else
    replay.gameVersion = 22081;
#endif
    replay.framerate = 240.0;
    replay.ldm = false;
    replay.platformer = false;
    replay.coins = coins;
    replay.levelInfo.id = static_cast<uint32_t>(levelId);
    replay.levelInfo.name = levelName;

    std::vector<p1::InputCmd> sortedPlan = plan;
    std::stable_sort(sortedPlan.begin(), sortedPlan.end(), [](const auto& a, const auto& b) {
        if (a.p2 != b.p2) return !a.p2 && b.p2;
        return a.step < b.step;
    });

    uint64_t maxFrame = 0;
    for (const auto& cmd : sortedPlan) {
        // frameOffset shifts every input by whole frames, for a player that counts frames differently
        // from the mod (the mod applies step N just before physics step N, counting from 0).
        uint64_t frame = static_cast<uint64_t>(std::max(0, cmd.step + frameOffset));
        if (frame > maxFrame) maxFrame = frame;
        replay.inputs.emplace_back(frame, static_cast<uint8_t>(1) /* Jump */, cmd.p2, cmd.down);
    }

    const uint64_t durFrames = std::max<uint64_t>(maxFrame, static_cast<uint64_t>(std::max(0, totalTicks)));
    replay.duration = static_cast<float>(durFrames) / 240.0f;

    auto res = replay.exportData(fullPath);
    if (res.isErr()) {
        outError = res.unwrapErr();
        return false;
    }

    return true;
}

} // namespace gdsolver::macro
