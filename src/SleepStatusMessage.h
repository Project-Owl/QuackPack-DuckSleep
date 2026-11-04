/**
 * @file SleepStatusMessage.h
 * @brief Builds the sleepstatus JSON payload (topic DUCK_SLEEP_STATUS_TOPIC).
 *
 * One flat object, snake_case codes for state, reason and wake:
 *
 *   {"state":"sleeping","reason":"sleep_window","planS":28710,
 *    "wakeAt":1767337110,"wake":"cold","mv":3912}
 *   {"state":"awake","reason":"sleep_window","sleptS":28650,"chunks":8,
 *    "wake":"schedule_end","mv":3874}
 *
 * - planS / wakeAt: sleeping only; wakeAt is left out when the clock is unknown
 * - sleptS: awake only, when both ends of the sleep had a clock
 * - chunks: awake only; more than ceil(planS / DUCK_SLEEP_MAX_CHUNK_S) means early timer wakes
 * - mv: cell voltage in mV, null when there is no plausible reading
 * - error: "pmu_unavailable" when the sketch reports its PMU is not answering
 *
 * Bounded: the result is never longer than kMaxPayload (CDP MAX_DATA_LENGTH).
 */
#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "SleepPolicy.h"

namespace duckSleep {

/// CDP MAX_DATA_LENGTH. Kept here so the library does not need CDP headers.
constexpr std::size_t kMaxPayload = 228;

/// PMU state as reported by the sketch.
enum class PmuState : std::uint8_t { Unknown = 0, Ok = 1, Unavailable = 2 };

struct StatusFields {
    bool sleeping = false;
    const char* reason = "none";   ///< reasonName() when sleeping; see awakeReasonName() when awake
    WakeCause wake = WakeCause::ColdBoot;
    std::uint32_t plannedS = 0;    ///< sleeping: until the planned wake or the next battery check
    std::int64_t wakeAtEpoch = 0;  ///< sleeping: absolute planned wake, 0 if unknown
    std::int64_t sleptS = -1;      ///< awake: seconds asleep, < 0 if unknown
    std::uint16_t chunks = 0;      ///< awake: deep sleeps in the sleep that just ended
    std::int32_t batteryMv = 0;    ///< <= 0 or implausible: null
    PmuState pmu = PmuState::Unknown;
};

/// The reason an awake status gives for the sleep that just ended.
inline const char* awakeReasonName(WakeCause cause) {
    switch (cause) {
    case WakeCause::ScheduleEnd: return "sleep_window";
    case WakeCause::BatteryRecovered:
    case WakeCause::BatteryCheck: return "battery_recovered";
    case WakeCause::Reset: return "interrupted";
    case WakeCause::ClockLost: return "clock_lost";
    case WakeCause::ColdBoot: return "cold_boot";
    }
    return "unknown";
}

/**
 * @brief Serialises @p f into @p out.
 * @returns the payload length, or 0 if it does not fit in @p capacity - 1 or
 *          kMaxPayload bytes (then @p out holds an empty string).
 */
inline std::size_t sleepStatusJson(const StatusFields& f, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return 0;
    out[0] = '\0';

    JsonDocument doc;
    doc["state"] = f.sleeping ? "sleeping" : "awake";
    doc["reason"] = f.reason != nullptr ? f.reason : "none";
    if (f.sleeping) {
        doc["planS"] = f.plannedS;
        if (f.wakeAtEpoch > 0) doc["wakeAt"] = f.wakeAtEpoch;
    } else {
        if (f.sleptS >= 0) doc["sleptS"] = f.sleptS;
        doc["chunks"] = f.chunks;
    }
    doc["wake"] = wakeCauseName(f.wake);
    if (batteryPlausible(f.batteryMv)) {
        doc["mv"] = f.batteryMv;
    } else {
        doc["mv"] = nullptr;
    }
    if (f.pmu == PmuState::Unavailable) doc["error"] = "pmu_unavailable";

    if (doc.overflowed()) return 0;
    const std::size_t length = measureJson(doc);
    if (length > kMaxPayload || length + 1 > capacity) return 0;
    return serializeJson(doc, out, capacity);
}

}  // namespace duckSleep
