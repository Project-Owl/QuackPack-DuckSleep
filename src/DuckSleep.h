/**
 * @file DuckSleep.h
 * @brief Opt-in scheduled deep sleep and battery survival hold for MamaDucks.
 *
 * A sketch calls three functions, always, whether sleep is built in or not:
 *
 *   void setup() {
 *     duckSleep::earlyBoot();            // first line: may go straight back to sleep
 *     ...PMU, rails, CDP setup...
 *     duckSleep::begin(hooks, idHash);   // after CDP setup
 *   }
 *   void loop() {
 *     duck.run();
 *     duckSleep::loop();
 *     if (duckSleep::sleepPending()) return;   // no new telemetry while a sleep is armed
 *     ...
 *   }
 *
 * With DUCK_SLEEP_ENABLED=0 and DUCK_SURVIVAL_ENABLED=0 (the defaults) these
 * are empty inline functions and cost no flash. PapaDucks and DetectorDucks
 * must never enable it: they are the gateway, the time source and the
 * command entry point.
 *
 * Nothing here depends on CDP: everything CDP-specific comes in through Hooks.
 * This header includes no Arduino headers either.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "DuckSleepConfig.h"

namespace duckSleep {

/**
 * @brief What DuckSleep needs from the sketch. Every hook is optional; a
 * missing hook has the fallback described next to it. Captureless lambdas
 * convert to these function pointers.
 */
struct Hooks {
    /// True when nothing is queued for or on the air (CDP 5.5: Duck::txIdle()).
    /// Missing: fixed waits (4 s before the status packet, 15 s after it).
    bool (*txIdle)() = nullptr;
    /// Put the LoRa radio to sleep before the rails go off (CDP 5.5: Duck::prepareForSleep()).
    /// Missing: skipped.
    void (*prepareRadioForSleep)() = nullptr;
    /// Send a payload on the mesh, e.g. duck.sendData(topic, std::string(json)) == DUCK_ERR_NONE.
    /// Missing: no sleepstatus packets.
    bool (*sendStatus)(std::uint8_t topic, const char* json) = nullptr;
    /// Cell voltage in mV, <= 0 when unknown. Missing or implausible: the survival hold
    /// never starts (fail awake).
    std::int32_t (*batteryMv)() = nullptr;
    /// Extra gate on the system clock, e.g. Duck::hasValidTime(). Missing: the
    /// 2025 floor and DUCK_BUILD_EPOCH only.
    bool (*timeValid)() = nullptr;
    /// Whether the PMU answers; reported in sleepstatus. Missing: not reported.
    bool (*pmuAvailable)() = nullptr;
    /// Called with false right before deep sleep (cut GPS, sensor and LoRa rails,
    /// hold load-switch pins) and with true from begin() after a DuckSleep wake.
    /// Rails the radio needs during setup must be restored before CDP setup.
    void (*powerGate)(bool on) = nullptr;
};

/// Maximum number of extra power gates (see addPowerGate()).
constexpr std::size_t kMaxExtraPowerGates = 4;

/// FNV-1a of a device ID, for begin(). Same input, same jitter.
/// Example: auto id = duck.getDuckId(); duckSleep::deviceIdHash(id.data(), id.size());
inline std::uint32_t deviceIdHash(const void* data, std::size_t length) { return hashId(data, length); }

/// True when DuckSleep is built into this firmware.
constexpr bool kBuiltIn = DUCK_SLEEP_ACTIVE != 0;

#if DUCK_SLEEP_ACTIVE

/**
 * @brief First line of setup(). After a deep-sleep timer wake that is early,
 * or a chunk boundary, or a survival hold whose cell is still low, it goes
 * straight back to deep sleep: no rails, no radio, no CDP, no packets.
 * Otherwise it classifies the boot and returns.
 *
 * @param earlyBatteryMv optional reader usable before setup (no rails needed),
 *        e.g. AXP2101 over I2C. Without it a survival-hold wake boots fully
 *        and loop() checks the battery through Hooks::batteryMv.
 */
void earlyBoot(std::int32_t (*earlyBatteryMv)() = nullptr);

/**
 * @brief Call once after CDP setup. Calls earlyBoot() itself if the sketch did not.
 * @param hooks copied; may be changed later only by calling begin() again
 * @param idHash a stable hash of the device ID (deviceIdHash()), for the wake jitter
 */
void begin(const Hooks& hooks, std::uint32_t idHash);

/// Call on every loop() pass. Never blocks; it may not return when the duck goes to sleep.
void loop();

/// A sleep is armed and the status/flush sequence is running. Skip new telemetry.
bool sleepPending();

/// Why this boot happened (ColdBoot when nothing was armed). For boot payloads.
WakeCause lastWakeCause();

/// Deep sleeps in the sleep that just ended (0 after a cold boot). For boot payloads.
std::uint16_t lastSleepChunks();

/// Register another power gate (e.g. from another QuackPack). Called after
/// Hooks::powerGate, in registration order. @returns false when full.
bool addPowerGate(void (*gate)(bool on));

#else  // DUCK_SLEEP_ACTIVE: everything compiles to nothing.

inline void earlyBoot(std::int32_t (*)() = nullptr) {}
inline void begin(const Hooks&, std::uint32_t) {}
inline void loop() {}
inline bool sleepPending() { return false; }
inline WakeCause lastWakeCause() { return WakeCause::ColdBoot; }
inline std::uint16_t lastSleepChunks() { return 0; }
inline bool addPowerGate(void (*)(bool)) { return false; }

#endif  // DUCK_SLEEP_ACTIVE

}  // namespace duckSleep
