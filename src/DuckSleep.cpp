/**
 * @file DuckSleep.cpp
 * @brief ESP32 adapter: retained state, deep sleep, the status and flush
 * sequence, and the hooks. Every decision comes from SleepPolicy.h.
 *
 * Compiled only when DUCK_SLEEP_ENABLED or DUCK_SURVIVAL_ENABLED is 1;
 * otherwise this file is empty and DuckSleep.h provides inline no-ops.
 */
#include "DuckSleep.h"

#if DUCK_SLEEP_ACTIVE

#if !defined(ARDUINO_ARCH_ESP32)
#error "DuckSleep needs an ESP32: it uses deep sleep and RTC memory"
#endif

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_rom_sys.h>
#include <esp_sleep.h>

#include <cstring>
#include <ctime>
#include <type_traits>

#include "SleepStatusMessage.h"

namespace duckSleep {
namespace {

// ---- Timing of the sleep entry sequence ----

constexpr std::uint32_t kEvalIntervalMs = 60000;      ///< policy check while awake
constexpr std::uint32_t kAwakeStatusDelayMs = 10000;  ///< after begin(), plus 0..5 s by device
constexpr std::uint32_t kStatusRetryMs = 5000;
constexpr std::uint8_t kStatusTries = 3;
constexpr std::uint32_t kIdleSettleMs = 500;          ///< txIdle() must hold this long
constexpr std::uint32_t kIdleWaitMaxMs = 10000;       ///< wait for a quiet channel before the status, at most
constexpr std::uint32_t kFlushMinMs = 1000;           ///< after the status, give CDP time to queue it
constexpr std::uint32_t kFlushMaxMs = 20000;          ///< ...and never wait longer than this
// Without a txIdle() hook: the fixed gaps Phoenix used (SLEEP_STATUS_TX_GAP_MS, SLEEP_TX_FLUSH_MS).
constexpr std::uint32_t kFallbackGapMs = 4000;
constexpr std::uint32_t kFallbackFlushMs = 15000;

static_assert(std::is_trivially_copyable<Retained>::value, "Retained must be plain data");

// Bytes, not a Retained object: no constructor can ever run over them at
// start-up. Only a power-on reloads the zero image; the magic word guards the rest.
RTC_DATA_ATTR std::uint8_t rtcBytes[sizeof(Retained)];

Retained loadRetained() {
    Retained r;
    std::memcpy(&r, rtcBytes, sizeof(r));
    return r;
}

void storeRetained(const Retained& r) { std::memcpy(rtcBytes, &r, sizeof(r)); }

enum class Phase : std::uint8_t {
    Awake,             ///< checking the policy every kEvalIntervalMs
    WaitBeforeStatus,  ///< a sleep is armed; waiting for a quiet channel
    Flushing,          ///< status queued; waiting for CDP to get it on the air
};

struct Runtime {
    Hooks hooks;
    void (*extraGates[kMaxExtraPowerGates])(bool) = {};
    std::size_t extraGateCount = 0;
    bool earlyDone = false;
    bool begun = false;
    BootDecision boot;
    std::uint32_t idHash = 0;
    bool holdActive = false;
    LowBatteryDebounce debounce;
    Phase phase = Phase::Awake;
    Decision pending;
    bool statusOnSleep = false;
    std::uint32_t phaseStartMs = 0;
    bool idleSeen = false;
    std::uint32_t idleSinceMs = 0;
    std::uint32_t beginMs = 0;
    bool evaluatedOnce = false;
    std::uint32_t lastEvalMs = 0;
    Reason lastReason = Reason::Disabled;
    bool awakeStatusDue = false;
    std::uint32_t awakeStatusSinceMs = 0;
    std::uint32_t awakeStatusDelayMs = 0;
    std::uint8_t awakeStatusTries = 0;
};

Runtime g;

bool elapsed(std::uint32_t now, std::uint32_t since, std::uint32_t ms) {
    return std::uint32_t(now - since) >= ms;
}

std::int64_t clockNow() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::int32_t readBattery() { return g.hooks.batteryMv != nullptr ? g.hooks.batteryMv() : 0; }

PmuState pmuState() {
    if (g.hooks.pmuAvailable == nullptr) return PmuState::Unknown;
    return g.hooks.pmuAvailable() ? PmuState::Ok : PmuState::Unavailable;
}

/// True once txIdle() has held for kIdleSettleMs, so a gap between CDP
/// queueing a packet and keying the radio does not count as idle.
bool txIdleSettled(std::uint32_t now) {
    if (!g.hooks.txIdle()) {
        g.idleSeen = false;
        return false;
    }
    if (!g.idleSeen) {
        g.idleSeen = true;
        g.idleSinceMs = now;
    }
    return elapsed(now, g.idleSinceMs, kIdleSettleMs);
}

bool sendStatus(const StatusFields& fields) {
    if (g.hooks.sendStatus == nullptr) return false;
    char json[kMaxPayload + 1];
    if (sleepStatusJson(fields, json, sizeof(json)) == 0) return false;
    const bool ok = g.hooks.sendStatus(kStatusTopic, json);
    Serial.printf("[SLEEP] Status %s: %s\n", ok ? "queued" : "send failed", json);
    return ok;
}

void runPowerGates(bool on) {
    if (g.hooks.powerGate != nullptr) g.hooks.powerGate(on);
    for (std::size_t i = 0; i < g.extraGateCount; ++i) g.extraGates[i](on);
}

[[noreturn]] void deepSleepFor(std::uint32_t seconds) {
    esp_sleep_enable_timer_wakeup(static_cast<std::uint64_t>(seconds) * 1000000ULL);
    esp_deep_sleep_start();
}

void scheduleAwakeStatus(std::uint32_t now) {
    g.awakeStatusDue = g.hooks.sendStatus != nullptr;
    g.awakeStatusSinceMs = now;
    g.awakeStatusDelayMs = kAwakeStatusDelayMs + g.idHash % 5000u;
    g.awakeStatusTries = 0;
}

void sendAwakeStatus(std::uint32_t now) {
    const Config config = configFromDefines();
    const Retained& slept = g.boot.finished;
    StatusFields f;
    f.sleeping = false;
    f.wake = g.boot.cause;
    f.reason = awakeReasonName(g.boot.cause);
    f.chunks = slept.chunks;
    const std::int64_t epoch = clockNow();
    if (slept.sleptAtEpoch > 0 && clockUsable(config, epoch) && epoch >= slept.sleptAtEpoch) {
        f.sleptS = epoch - slept.sleptAtEpoch;
    }
    f.batteryMv = readBattery();
    f.pmu = pmuState();
    if (sendStatus(f) || ++g.awakeStatusTries >= kStatusTries) {
        g.awakeStatusDue = false;
        return;
    }
    g.awakeStatusSinceMs = now;
    g.awakeStatusDelayMs = kStatusRetryMs;
}

void sendSleepingStatus() {
    StatusFields f;
    f.sleeping = true;
    f.reason = reasonName(g.pending.reason);
    f.wake = g.boot.cause;
    f.plannedS = g.pending.totalS;
    f.wakeAtEpoch = g.pending.wakeAtEpoch;
    f.batteryMv = readBattery();
    f.pmu = pmuState();
    sendStatus(f);  // one try: the duck is leaving either way
}

void arm(const Decision& d, std::uint32_t now, bool withStatus) {
    g.pending = d;
    g.statusOnSleep = withStatus && g.hooks.sendStatus != nullptr;
    g.phase = Phase::WaitBeforeStatus;
    g.phaseStartMs = now;
    g.idleSeen = false;
    g.awakeStatusDue = false;  // the sleeping status supersedes it
    Serial.printf("[SLEEP] Armed: %s, %lu s planned, wake at %lld\n", reasonName(d.reason),
                  static_cast<unsigned long>(d.totalS), static_cast<long long>(d.wakeAtEpoch));
}

void evaluate(std::uint32_t now) {
    const Config config = configFromDefines();
    AwakeInput in;
    in.utcEpoch = clockNow();
    in.timeValid = g.hooks.timeValid == nullptr || g.hooks.timeValid();
    in.batteryMv = config.survivalEnabled ? readBattery() : 0;
    in.holdActive = g.holdActive;
    in.graceActive = (g.boot.cause == WakeCause::ColdBoot || g.boot.cause == WakeCause::Reset) &&
                     !elapsed(now, g.beginMs, kInstallGraceS * 1000u);
    in.idHash = g.idHash;
    const Decision d = decide(config, in);

    // Entering the hold needs two low readings in a row; staying in it does not.
    if (d.action == Action::SurvivalHold && !g.holdActive) {
        if (!g.debounce.update(true)) {
            Serial.printf("[SLEEP] Battery %ld mV at or below %ld mV; confirming at the next check\n",
                          static_cast<long>(in.batteryMv), static_cast<long>(config.survivalSleepMv));
            return;
        }
    } else {
        g.debounce.update(false);
    }

    if (g.holdActive && d.action != Action::SurvivalHold && batteryPlausible(in.batteryMv)) {
        // Booted from a survival hold without an early reading: the cell recovered.
        g.holdActive = false;
        Serial.printf("[SLEEP] Battery recovered: %ld mV\n", static_cast<long>(in.batteryMv));
        scheduleAwakeStatus(now);
    }

    if (d.action == Action::StayAwake) {
        if (d.reason != g.lastReason) {
            Serial.printf("[SLEEP] Awake: %s\n", reasonName(d.reason));
            g.lastReason = d.reason;
        }
        return;
    }
    g.lastReason = d.reason;
    // Still in a hold after a boot that could not read the battery early: no
    // new status packet every recheck, just go back to sleep.
    arm(d, now, !g.holdActive);
}

void enterSleep(std::uint32_t now) {
    const Config config = configFromDefines();
    Decision d = g.pending;
    const std::int64_t epoch = clockNow();
    if (d.action == Action::Sleep) {
        // The target is absolute; re-plan the first chunk against the clock after the flush.
        const std::int64_t remaining = d.wakeAtEpoch - epoch;
        if (!clockUsable(config, epoch) || remaining < static_cast<std::int64_t>(kMinSleepS)) {
            Serial.println("[SLEEP] Sleep cancelled: the window ended or the clock changed during the flush");
            g.phase = Phase::Awake;
            g.lastEvalMs = now;
            return;
        }
        d.sleepS = minU32(static_cast<std::uint32_t>(remaining), config.maxChunkS);
    }

    Retained r;
    armRetained(r, d, epoch, g.idHash, false);
    storeRetained(r);

    if (g.hooks.prepareRadioForSleep != nullptr) g.hooks.prepareRadioForSleep();
    runPowerGates(false);
    Serial.printf("[SLEEP] Deep sleep for %lu s (%s)\n", static_cast<unsigned long>(d.sleepS),
                  reasonName(d.reason));
    Serial.flush();
    deepSleepFor(d.sleepS);
}

void loopWaitBeforeStatus(std::uint32_t now) {
    bool ready;
    if (!g.statusOnSleep) {
        ready = true;
    } else if (g.hooks.txIdle != nullptr) {
        ready = txIdleSettled(now) || elapsed(now, g.phaseStartMs, kIdleWaitMaxMs);
    } else {
        ready = elapsed(now, g.phaseStartMs, kFallbackGapMs);
    }
    if (!ready) return;
    if (g.statusOnSleep) sendSleepingStatus();
    g.phase = Phase::Flushing;
    g.phaseStartMs = now;
    g.idleSeen = false;
}

void loopFlushing(std::uint32_t now) {
    bool ready;
    if (g.hooks.txIdle != nullptr) {
        ready = (elapsed(now, g.phaseStartMs, kFlushMinMs) && txIdleSettled(now)) ||
                elapsed(now, g.phaseStartMs, kFlushMaxMs);
    } else {
        ready = elapsed(now, g.phaseStartMs, kFallbackFlushMs);
    }
    if (ready) enterSleep(now);
}

void loopAwake(std::uint32_t now) {
    if (g.awakeStatusDue && elapsed(now, g.awakeStatusSinceMs, g.awakeStatusDelayMs)) {
        sendAwakeStatus(now);
    }
    if (g.evaluatedOnce && !elapsed(now, g.lastEvalMs, kEvalIntervalMs)) return;
    g.evaluatedOnce = true;
    g.lastEvalMs = now;
    evaluate(now);
}

}  // namespace

void earlyBoot(std::int32_t (*earlyBatteryMv)()) {
    if (g.earlyDone) return;
    g.earlyDone = true;

    const Config config = configFromDefines();
    Retained r = loadRetained();
    BootInput in;
    in.timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    in.utcEpoch = clockNow();
    if (in.timerWake && r.magic == kRetainedMagic && config.survivalEnabled && earlyBatteryMv != nullptr) {
        in.batteryMv = earlyBatteryMv();
    }
    g.boot = decideBoot(config, r, in);
    storeRetained(r);
    if (g.boot.resleep) {
        // Serial is not started yet; the ROM printer still reaches UART0.
        esp_rom_printf("[SLEEP] Timer wake, still asleep: %u s more (deep sleep %u)\n",
                       static_cast<unsigned>(g.boot.sleepS), static_cast<unsigned>(r.chunks));
        deepSleepFor(g.boot.sleepS);
    }
    g.holdActive = g.boot.holdActive;
}

void begin(const Hooks& hooks, std::uint32_t idHash) {
    if (!g.earlyDone) earlyBoot(nullptr);
    g.hooks = hooks;
    g.idHash = idHash;
    if (g.begun) return;  // hooks replaced; the running state stays
    g.begun = true;

    const std::uint32_t now = millis();
    g.beginMs = now;
    const WakeCause cause = g.boot.cause;
    if (cause != WakeCause::ColdBoot) runPowerGates(true);
    // The awake status reports the sleep that just ended. After a survival
    // wake without an early reading it waits until loop() sees the cell recovered.
    if (cause != WakeCause::ColdBoot && cause != WakeCause::BatteryCheck) scheduleAwakeStatus(now);

    const Config config = configFromDefines();
    const Window window = effectiveWindow(config);
    Serial.printf("[SLEEP] Wake cause %s, deep sleeps %u\n", wakeCauseName(cause),
                  static_cast<unsigned>(g.boot.finished.chunks));
    if (config.scheduleEnabled) {
        if (window.valid) {
            Serial.printf("[SLEEP] Schedule %02u:%02u-%02u:%02u UTC as %s, jitter %lu s\n",
                          window.startMin / 60, window.startMin % 60, window.wakeMin / 60, window.wakeMin % 60,
                          config.role == Role::Relay ? "relay" : "leaf",
                          static_cast<unsigned long>(wakeJitterS(idHash, config.wakeJitterS)));
        } else {
            Serial.println("[SLEEP] Schedule enabled but no usable window: never sleeps on schedule");
        }
    }
    if (config.survivalEnabled) {
        Serial.printf("[SLEEP] Survival hold at %ld mV, release at %ld mV, recheck %u min%s\n",
                      static_cast<long>(config.survivalSleepMv), static_cast<long>(config.survivalWakeMv),
                      static_cast<unsigned>(config.survivalRecheckMin),
                      hooks.batteryMv == nullptr ? " (no battery hook: never holds)" : "");
    }
}

void loop() {
    if (!g.begun) return;
    const std::uint32_t now = millis();
    switch (g.phase) {
    case Phase::Awake:
        loopAwake(now);
        break;
    case Phase::WaitBeforeStatus:
        loopWaitBeforeStatus(now);
        break;
    case Phase::Flushing:
        loopFlushing(now);
        break;
    }
}

bool sleepPending() { return g.phase != Phase::Awake; }

WakeCause lastWakeCause() { return g.boot.cause; }

std::uint16_t lastSleepChunks() { return g.boot.finished.chunks; }

bool addPowerGate(void (*gate)(bool on)) {
    if (gate == nullptr || g.extraGateCount >= kMaxExtraPowerGates) return false;
    g.extraGates[g.extraGateCount++] = gate;
    return true;
}

}  // namespace duckSleep

#endif  // DUCK_SLEEP_ACTIVE
