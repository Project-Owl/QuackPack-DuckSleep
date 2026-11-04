/**
 * @file SleepPolicy.h
 * @brief Pure sleep decisions for DuckSleep.
 *
 * No Arduino, ESP-IDF or ClusterDuck Protocol headers: the clock, the battery
 * reading and the state kept across deep sleep all come in as plain values, so
 * the host tests can drive every decision. The ESP32 adapter (DuckSleep.cpp)
 * only applies what these functions return.
 *
 * Two questions are answered here:
 *  - decide():     while awake, should this duck stay awake, sleep on the
 *                  schedule, or hold in survival sleep?
 *  - decideBoot(): right after a deep-sleep wake, before any rail or radio is
 *                  powered, is the sleep really over, or should the duck go
 *                  straight back to sleep without anyone noticing?
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace duckSleep {

// ---- Fixed behaviour (deliberately not configurable by define) ----

/// Any clock before 2025-01-01T00:00:00Z is treated as "no time".
constexpr std::int64_t kMinSaneEpoch = 1735689600;
constexpr std::uint32_t kSecondsPerDay = 86400;
constexpr std::uint16_t kMinutesPerDay = 1440;
/// Relays fall asleep this many minutes after the leaves and wake this many before them.
constexpr std::uint16_t kRelayGuardMin = 10;
/// A sleep shorter than this is not worth a reboot; the duck stays awake instead.
constexpr std::uint32_t kMinSleepS = 60;
/// No scheduled sleep during the first minutes after a cold boot or an
/// unplanned reset, so an installer sees the boot traffic. Survival is not delayed.
constexpr std::uint32_t kInstallGraceS = 300;
/// Battery readings outside this range mean "no cell" or "bad reading": fail awake.
constexpr std::int32_t kBatteryMinMv = 3000;
constexpr std::int32_t kBatteryMaxMv = 4500;
/// Marks the retained state as written by this library ("DSLP").
constexpr std::uint32_t kRetainedMagic = 0x44534C50;

enum class Role : std::uint8_t { Leaf = 0, Relay = 1 };

/// Everything the policy needs to know about the build. See DuckSleepConfig.h
/// for the defines that fill it.
struct Config {
    bool scheduleEnabled = false;
    std::uint16_t startMinUtc = 0;   ///< leaves fall asleep at this UTC minute of day
    std::uint16_t wakeMinUtc = 0;    ///< leaves are awake again by this UTC minute; equal to start: no window
    Role role = Role::Relay;
    std::uint16_t wakeJitterS = 90;  ///< wake up to this many seconds early, spread by device ID
    std::uint32_t maxChunkS = 3600;  ///< longest single deep sleep
    bool survivalEnabled = false;
    std::int32_t survivalSleepMv = 3400;  ///< hold when the cell is at or below this
    std::int32_t survivalWakeMv = 3700;   ///< leave the hold only at or above this
    std::uint16_t survivalRecheckMin = 30;
    std::int64_t buildEpoch = 0;     ///< a clock older than the firmware is wrong
};

/// What the last sleep was for. Stored as a raw byte in Retained::mode.
enum class Mode : std::uint8_t { None = 0, Scheduled = 1, Survival = 2 };

/// State that survives deep sleep (RTC slow memory on the ESP32). Plain data
/// so it can be placed in RTC memory and built by the tests. Every field is
/// trusted only when magic == kRetainedMagic.
struct Retained {
    std::uint32_t magic = 0;
    std::uint8_t mode = 0;           ///< a Mode value
    std::uint16_t chunks = 0;        ///< deep sleeps so far in this sleep, including silent re-sleeps
    std::uint32_t idHash = 0;        ///< device ID hash, for the jitter after a silent re-sleep
    std::uint32_t plannedS = 0;      ///< total planned length when the sleep was armed
    std::int64_t sleptAtEpoch = 0;   ///< when the sleep began; 0 if the clock was unknown
    std::int64_t wakeAtEpoch = 0;    ///< Scheduled: when to be awake. Survival: 0 or the next recheck
};

/// Why this boot happened, as far as DuckSleep is concerned.
enum class WakeCause : std::uint8_t {
    ColdBoot = 0,        ///< power-on, or any reset without a DuckSleep sleep armed
    Reset = 1,           ///< a sleep was armed but something other than the timer woke the duck
    ScheduleEnd = 2,     ///< the scheduled sleep is over
    ClockLost = 3,       ///< woke from a scheduled sleep but the clock can no longer be trusted
    BatteryRecovered = 4,///< the survival hold ended: the cell is at or above the wake threshold
    BatteryCheck = 5,    ///< survival-hold wake with no early battery reading; loop() decides
};

/// Short snake_case name for payloads and logs.
inline const char* wakeCauseName(WakeCause cause) {
    switch (cause) {
    case WakeCause::ColdBoot: return "cold";
    case WakeCause::Reset: return "reset";
    case WakeCause::ScheduleEnd: return "schedule_end";
    case WakeCause::ClockLost: return "clock_lost";
    case WakeCause::BatteryRecovered: return "battery_recovered";
    case WakeCause::BatteryCheck: return "battery_check";
    }
    return "unknown";
}

enum class Action : std::uint8_t { StayAwake = 0, Sleep = 1, SurvivalHold = 2 };

enum class Reason : std::uint8_t {
    Disabled = 0,      ///< scheduled sleep is not built in
    NoWindow,          ///< start == wake, or a relay window shorter than both guards
    NoTime,            ///< no usable clock: never sleep on schedule
    OldEpoch,          ///< the clock is older than the firmware build
    Grace,             ///< first minutes after a cold boot or reset
    OutsideWindow,
    WindowEnding,      ///< inside the window, but less than kMinSleepS of it is left
    SleepWindow,       ///< sleep on schedule
    BatteryLow,        ///< enter the survival hold
    BatteryHold,       ///< stay in the survival hold
};

/// Short snake_case name for payloads and logs.
inline const char* reasonName(Reason reason) {
    switch (reason) {
    case Reason::Disabled: return "disabled";
    case Reason::NoWindow: return "no_window";
    case Reason::NoTime: return "no_time";
    case Reason::OldEpoch: return "old_epoch";
    case Reason::Grace: return "grace";
    case Reason::OutsideWindow: return "outside_window";
    case Reason::WindowEnding: return "window_ending";
    case Reason::SleepWindow: return "sleep_window";
    case Reason::BatteryLow: return "battery_low";
    case Reason::BatteryHold: return "battery_hold";
    }
    return "unknown";
}

struct Decision {
    Action action = Action::StayAwake;
    Reason reason = Reason::Disabled;
    std::uint32_t sleepS = 0;     ///< this deep sleep, at most Config::maxChunkS
    std::uint32_t totalS = 0;     ///< until the planned wake (schedule) or the recheck (survival)
    std::int64_t wakeAtEpoch = 0; ///< absolute planned wake; 0 when the clock is unknown
};

/// Inputs for decide(). The adapter fills them from the system clock, the
/// battery hook and its own RAM state.
struct AwakeInput {
    std::int64_t utcEpoch = 0;   ///< system clock, UTC seconds
    bool timeValid = true;       ///< optional extra gate from the sketch (e.g. Duck::hasValidTime())
    std::int32_t batteryMv = 0;  ///< <= 0 when unknown
    bool holdActive = false;     ///< in a survival hold: leaving it needs survivalWakeMv
    bool graceActive = false;    ///< within kInstallGraceS of a cold boot or reset
    std::uint32_t idHash = 0;
};

/// Inputs for decideBoot(), all available before any peripheral is powered.
struct BootInput {
    bool timerWake = false;      ///< the deep-sleep timer woke the chip
    std::int64_t utcEpoch = 0;   ///< system clock; on the ESP32 it runs on through deep sleep
    std::int32_t batteryMv = 0;  ///< early reading, <= 0 when not available
};

struct BootDecision {
    bool resleep = false;         ///< go straight back to sleep: no rails, no radio, no packets
    std::uint32_t sleepS = 0;     ///< length of that sleep
    WakeCause cause = WakeCause::ColdBoot;
    bool holdActive = false;      ///< the survival hold may still be on; loop() checks the battery first
    Retained finished;            ///< the sleep that just ended (magic 0 if none), for the awake status
};

// ---- Helpers ----

inline std::uint32_t minU32(std::uint32_t a, std::uint32_t b) { return a < b ? a : b; }

/// FNV-1a over the device ID, so the same duck always gets the same jitter.
inline std::uint32_t hashId(const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; bytes != nullptr && i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

inline bool batteryPlausible(std::int32_t mv) {
    return mv >= kBatteryMinMv && mv <= kBatteryMaxMv;
}

/// The clock is usable for scheduling: after 2025 and not older than the build.
inline bool clockUsable(const Config& config, std::int64_t epoch) {
    return epoch >= kMinSaneEpoch && epoch >= config.buildEpoch;
}

/// Seconds the duck wakes early, 0..maxJitterS, fixed per device.
inline std::uint32_t wakeJitterS(std::uint32_t idHash, std::uint16_t maxJitterS) {
    return maxJitterS == 0 ? 0 : idHash % (static_cast<std::uint32_t>(maxJitterS) + 1u);
}

/// The window this duck actually sleeps in, in UTC minutes of day.
struct Window {
    bool valid = false;
    std::uint16_t startMin = 0;
    std::uint16_t wakeMin = 0;
};

inline std::uint16_t windowLengthMin(std::uint16_t startMin, std::uint16_t wakeMin) {
    return static_cast<std::uint16_t>((wakeMin + kMinutesPerDay - startMin) % kMinutesPerDay);
}

/// Applies the relay guard: a relay sleeps kRelayGuardMin after the leaves and
/// wakes kRelayGuardMin before them, so it is there to carry their first and
/// last packets. A window too short for both guards never sleeps.
inline Window effectiveWindow(const Config& config) {
    Window window;
    if (!config.scheduleEnabled || config.startMinUtc >= kMinutesPerDay ||
        config.wakeMinUtc >= kMinutesPerDay || config.startMinUtc == config.wakeMinUtc) {
        return window;
    }
    std::uint16_t start = config.startMinUtc;
    std::uint16_t wake = config.wakeMinUtc;
    if (config.role == Role::Relay) {
        if (windowLengthMin(start, wake) <= 2 * kRelayGuardMin) return window;
        start = static_cast<std::uint16_t>((start + kRelayGuardMin) % kMinutesPerDay);
        wake = static_cast<std::uint16_t>((wake + kMinutesPerDay - kRelayGuardMin) % kMinutesPerDay);
    }
    window.valid = true;
    window.startMin = start;
    window.wakeMin = wake;
    return window;
}

/// Length of the effective window in seconds (0 when there is none). No single
/// sleep may be planned longer than this.
inline std::uint32_t windowLengthS(const Window& window) {
    return window.valid ? windowLengthMin(window.startMin, window.wakeMin) * 60u : 0;
}

inline std::uint32_t secondOfDay(std::int64_t epoch) {
    const std::int64_t s = epoch % static_cast<std::int64_t>(kSecondsPerDay);
    return static_cast<std::uint32_t>(s < 0 ? s + kSecondsPerDay : s);
}

/// Seconds until the effective window ends, or 0 when outside it.
inline std::uint32_t secondsUntilWindowEnd(const Window& window, std::int64_t epoch) {
    if (!window.valid) return 0;
    const std::uint32_t now = secondOfDay(epoch);
    const std::uint32_t start = window.startMin * 60u;
    const std::uint32_t wake = window.wakeMin * 60u;
    const bool inside = start < wake ? (now >= start && now < wake) : (now >= start || now < wake);
    if (!inside) return 0;
    return (wake + kSecondsPerDay - now) % kSecondsPerDay;
}

// ---- Decisions ----

/**
 * @brief While awake: stay awake, sleep on the schedule, or hold for the battery.
 *
 * The survival hold comes first and needs no clock; a missing or implausible
 * reading never puts the duck to sleep. Scheduled sleep needs a usable clock
 * and never plans past the end of the window, minus this duck's jitter.
 */
inline Decision decide(const Config& config, const AwakeInput& in) {
    Decision d;
    const bool clockOk = in.timeValid && clockUsable(config, in.utcEpoch);

    if (config.survivalEnabled && batteryPlausible(in.batteryMv)) {
        const bool low = in.holdActive ? in.batteryMv < config.survivalWakeMv
                                       : in.batteryMv <= config.survivalSleepMv;
        if (low) {
            d.action = Action::SurvivalHold;
            d.reason = in.holdActive ? Reason::BatteryHold : Reason::BatteryLow;
            d.totalS = static_cast<std::uint32_t>(config.survivalRecheckMin) * 60u;
            d.sleepS = minU32(d.totalS, config.maxChunkS);
            d.wakeAtEpoch = clockOk ? in.utcEpoch + d.totalS : 0;
            return d;
        }
    }

    if (!config.scheduleEnabled) {
        d.reason = Reason::Disabled;
        return d;
    }
    const Window window = effectiveWindow(config);
    if (!window.valid) {
        d.reason = Reason::NoWindow;
        return d;
    }
    if (!in.timeValid || in.utcEpoch < kMinSaneEpoch) {
        d.reason = Reason::NoTime;
        return d;
    }
    if (in.utcEpoch < config.buildEpoch) {
        d.reason = Reason::OldEpoch;
        return d;
    }
    if (in.graceActive) {
        d.reason = Reason::Grace;
        return d;
    }
    const std::uint32_t untilEnd = secondsUntilWindowEnd(window, in.utcEpoch);
    if (untilEnd == 0) {
        d.reason = Reason::OutsideWindow;
        return d;
    }
    const std::uint32_t jitter = wakeJitterS(in.idHash, config.wakeJitterS);
    if (untilEnd <= jitter || untilEnd - jitter < kMinSleepS) {
        d.reason = Reason::WindowEnding;
        return d;
    }
    d.action = Action::Sleep;
    d.reason = Reason::SleepWindow;
    d.totalS = untilEnd - jitter;
    d.sleepS = minU32(d.totalS, config.maxChunkS);
    d.wakeAtEpoch = in.utcEpoch + d.totalS;
    return d;
}

/// Arms the retained state for a new sleep from a decide() result. Keeps the
/// start time and chunk count when one sleep turns into another (e.g. the
/// survival hold ends inside the window), so the awake status covers the whole night.
inline void armRetained(Retained& r, const Decision& d, std::int64_t nowEpoch,
                        std::uint32_t idHash, bool continuing) {
    if (!continuing) {
        r.chunks = 0;
        r.sleptAtEpoch = nowEpoch >= kMinSaneEpoch ? nowEpoch : 0;
    }
    r.magic = kRetainedMagic;
    r.mode = static_cast<std::uint8_t>(d.action == Action::Sleep ? Mode::Scheduled : Mode::Survival);
    r.chunks = static_cast<std::uint16_t>(r.chunks < 0xFFFF ? r.chunks + 1 : r.chunks);
    r.idHash = idHash;
    r.plannedS = d.totalS;
    r.wakeAtEpoch = d.wakeAtEpoch;
}

/**
 * @brief Right after boot, before rails, radio or CDP: is the sleep really over?
 *
 * Deep-sleep timer wakes can be early (the RC slow clock drifts; up to 18.5
 * minutes in an 8.5-hour sleep in the Phoenix soak), and long sleeps are split
 * into chunks. Waking just to fall asleep again must cost no rail, no radio
 * and no packet, so this decides it from the retained state alone.
 *
 * Updates @p retained: cleared when the duck stays awake, re-armed when it
 * sleeps again.
 */
inline BootDecision decideBoot(const Config& config, Retained& retained, const BootInput& in) {
    BootDecision out;
    const auto mode = static_cast<Mode>(retained.mode);
    if (retained.magic != kRetainedMagic || (mode != Mode::Scheduled && mode != Mode::Survival)) {
        retained = Retained{};
        out.cause = WakeCause::ColdBoot;
        return out;
    }
    if (!in.timerWake) {
        // Brownout, panic, button or upload while asleep: a normal boot.
        out.finished = retained;
        retained = Retained{};
        out.cause = WakeCause::Reset;
        return out;
    }

    if (mode == Mode::Scheduled) {
        const Window window = effectiveWindow(config);
        if (!clockUsable(config, in.utcEpoch) || !window.valid) {
            out.finished = retained;
            retained = Retained{};
            out.cause = WakeCause::ClockLost;
            return out;
        }
        const std::int64_t remaining = retained.wakeAtEpoch - in.utcEpoch;
        // Never sleep longer than the window: a target further away than that
        // means the clock moved under us. Fail awake.
        if (remaining > static_cast<std::int64_t>(windowLengthS(window))) {
            out.finished = retained;
            retained = Retained{};
            out.cause = WakeCause::ClockLost;
            return out;
        }
        if (remaining >= static_cast<std::int64_t>(kMinSleepS)) {
            retained.chunks = static_cast<std::uint16_t>(retained.chunks < 0xFFFF ? retained.chunks + 1 : retained.chunks);
            out.resleep = true;
            out.sleepS = minU32(static_cast<std::uint32_t>(remaining), config.maxChunkS);
            out.cause = WakeCause::ScheduleEnd;
            return out;
        }
        out.cause = WakeCause::ScheduleEnd;
    } else {
        if (!batteryPlausible(in.batteryMv)) {
            // No early reading: boot, and let loop() read the battery through
            // the hook with the hold's wake threshold still in force.
            out.finished = retained;
            retained = Retained{};
            out.cause = WakeCause::BatteryCheck;
            out.holdActive = true;
            return out;
        }
        if (in.batteryMv < config.survivalWakeMv) {
            retained.chunks = static_cast<std::uint16_t>(retained.chunks < 0xFFFF ? retained.chunks + 1 : retained.chunks);
            retained.plannedS = static_cast<std::uint32_t>(config.survivalRecheckMin) * 60u;
            retained.wakeAtEpoch = clockUsable(config, in.utcEpoch) ? in.utcEpoch + retained.plannedS : 0;
            out.resleep = true;
            out.sleepS = minU32(retained.plannedS, config.maxChunkS);
            out.cause = WakeCause::BatteryRecovered;
            return out;
        }
        out.cause = WakeCause::BatteryRecovered;
    }

    // This sleep is over. Does the policy want another one straight away
    // (cell low at the end of the window, or recovered inside it)?
    AwakeInput next;
    next.utcEpoch = in.utcEpoch;
    next.batteryMv = in.batteryMv;
    next.idHash = retained.idHash;
    const Decision d = decide(config, next);
    if (d.action != Action::StayAwake) {
        armRetained(retained, d, in.utcEpoch, retained.idHash, true);
        out.resleep = true;
        out.sleepS = d.sleepS;
        return out;
    }
    out.finished = retained;
    retained = Retained{};
    return out;
}

/// Two low readings in a row before entering the survival hold, so one sag
/// under transmit load does not put a relay to sleep.
struct LowBatteryDebounce {
    std::uint8_t streak = 0;
    /// @returns true when the hold should be entered now.
    bool update(bool low) {
        if (!low) {
            streak = 0;
            return false;
        }
        if (streak < 255) ++streak;
        return streak >= 2;
    }
};

}  // namespace duckSleep
