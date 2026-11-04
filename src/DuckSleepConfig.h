/**
 * @file DuckSleepConfig.h
 * @brief Build-time configuration for DuckSleep.
 *
 * Every value is a non-negative integer -D define with a default here, checked
 * with static_assert. Set them in build_flags (or PLATFORMIO_BUILD_FLAGS) so
 * the library sources see the same values as the sketch; a #define in the
 * sketch does not reach DuckSleep.cpp.
 *
 * With DUCK_SLEEP_ENABLED=0 and DUCK_SURVIVAL_ENABLED=0 (the defaults) the
 * whole library compiles away.
 */
#pragma once

#include "SleepPolicy.h"

/// 1: sleep in the daily UTC window below. 0: never sleep on a schedule.
#ifndef DUCK_SLEEP_ENABLED
#define DUCK_SLEEP_ENABLED 0
#endif

/// Leaves fall asleep at this UTC minute of day (0..1439).
#ifndef DUCK_SLEEP_START_MIN_UTC
#define DUCK_SLEEP_START_MIN_UTC 0
#endif

/// Leaves are awake again by this UTC minute of day (0..1439). Equal to the
/// start: no window. The window may cross midnight.
#ifndef DUCK_SLEEP_WAKE_MIN_UTC
#define DUCK_SLEEP_WAKE_MIN_UTC 0
#endif

/// 0 leaf, 1 relay. Relays (every CDP MamaDuck) fall asleep 10 minutes after
/// the leaves and wake 10 minutes before them.
#ifndef DUCK_SLEEP_ROLE
#define DUCK_SLEEP_ROLE 1
#endif

/// Wake up to this many seconds early, spread by device ID, so a mesh does not
/// wake in one burst (0..600; at most the relay guard, so relays stay first).
#ifndef DUCK_SLEEP_WAKE_JITTER_S
#define DUCK_SLEEP_WAKE_JITTER_S 90
#endif

/// Longest single deep sleep (60..86400 s). Each chunk wake recalibrates the
/// slow clock and checks the battery, without powering rails or radio.
#ifndef DUCK_SLEEP_MAX_CHUNK_S
#define DUCK_SLEEP_MAX_CHUNK_S 3600
#endif

/// 1: sleep while the cell is low, with or without a schedule or a clock.
#ifndef DUCK_SURVIVAL_ENABLED
#define DUCK_SURVIVAL_ENABLED 0
#endif

/// Enter the survival hold at or below this cell voltage (3000..4100 mV).
#ifndef DUCK_SURVIVAL_SLEEP_MV
#define DUCK_SURVIVAL_SLEEP_MV 3400
#endif

/// Leave the survival hold only at or above this cell voltage (3100..4200 mV,
/// above DUCK_SURVIVAL_SLEEP_MV).
#ifndef DUCK_SURVIVAL_WAKE_MV
#define DUCK_SURVIVAL_WAKE_MV 3700
#endif

/// Minutes between battery checks during the survival hold (5..720).
#ifndef DUCK_SURVIVAL_RECHECK_MIN
#define DUCK_SURVIVAL_RECHECK_MIN 30
#endif

/// CDP topic for the sleepstatus payload (0x10..0xFF). 0x1B is the Owl
/// sleepstatus topic that the DMS decodes by name.
#ifndef DUCK_SLEEP_STATUS_TOPIC
#define DUCK_SLEEP_STATUS_TOPIC 0x1B
#endif

/// UTC epoch of the build, e.g. -DDUCK_BUILD_EPOCH=$(date +%s). A clock older
/// than this is wrong (bad cmd_time, lost RTC) and never triggers a schedule.
/// 0: only the 2025-01-01 floor applies.
#ifndef DUCK_BUILD_EPOCH
#define DUCK_BUILD_EPOCH 0
#endif

static_assert(DUCK_SLEEP_ENABLED == 0 || DUCK_SLEEP_ENABLED == 1, "DUCK_SLEEP_ENABLED must be 0 or 1");
static_assert(DUCK_SLEEP_START_MIN_UTC >= 0 && DUCK_SLEEP_START_MIN_UTC <= 1439,
              "DUCK_SLEEP_START_MIN_UTC must be a UTC minute of day, 0..1439");
static_assert(DUCK_SLEEP_WAKE_MIN_UTC >= 0 && DUCK_SLEEP_WAKE_MIN_UTC <= 1439,
              "DUCK_SLEEP_WAKE_MIN_UTC must be a UTC minute of day, 0..1439");
static_assert(DUCK_SLEEP_ROLE == 0 || DUCK_SLEEP_ROLE == 1, "DUCK_SLEEP_ROLE must be 0 (leaf) or 1 (relay)");
static_assert(DUCK_SLEEP_WAKE_JITTER_S >= 0 && DUCK_SLEEP_WAKE_JITTER_S <= 600,
              "DUCK_SLEEP_WAKE_JITTER_S must be 0..600 so relays still wake before leaves");
static_assert(DUCK_SLEEP_MAX_CHUNK_S >= 60 && DUCK_SLEEP_MAX_CHUNK_S <= 86400,
              "DUCK_SLEEP_MAX_CHUNK_S must be 60..86400");
static_assert(DUCK_SURVIVAL_ENABLED == 0 || DUCK_SURVIVAL_ENABLED == 1, "DUCK_SURVIVAL_ENABLED must be 0 or 1");
static_assert(DUCK_SURVIVAL_SLEEP_MV >= 3000 && DUCK_SURVIVAL_SLEEP_MV <= 4100,
              "DUCK_SURVIVAL_SLEEP_MV must be 3000..4100");
static_assert(DUCK_SURVIVAL_WAKE_MV >= 3100 && DUCK_SURVIVAL_WAKE_MV <= 4200,
              "DUCK_SURVIVAL_WAKE_MV must be 3100..4200");
static_assert(DUCK_SURVIVAL_WAKE_MV > DUCK_SURVIVAL_SLEEP_MV,
              "DUCK_SURVIVAL_WAKE_MV must be above DUCK_SURVIVAL_SLEEP_MV");
static_assert(DUCK_SURVIVAL_RECHECK_MIN >= 5 && DUCK_SURVIVAL_RECHECK_MIN <= 720,
              "DUCK_SURVIVAL_RECHECK_MIN must be 5..720");
static_assert(DUCK_SLEEP_STATUS_TOPIC >= 0x10 && DUCK_SLEEP_STATUS_TOPIC <= 0xFF,
              "DUCK_SLEEP_STATUS_TOPIC must be a CDP application topic, 0x10..0xFF");
static_assert(DUCK_BUILD_EPOCH == 0 || (DUCK_BUILD_EPOCH >= 1704067200LL && DUCK_BUILD_EPOCH < 4102444800LL),
              "DUCK_BUILD_EPOCH must be 0 or a UTC epoch between 2024 and 2100");

/// True when any part of DuckSleep is built in.
#define DUCK_SLEEP_ACTIVE (DUCK_SLEEP_ENABLED || DUCK_SURVIVAL_ENABLED)

namespace duckSleep {

/// The defines above as a policy Config.
inline Config configFromDefines() {
    Config c;
    c.scheduleEnabled = DUCK_SLEEP_ENABLED == 1;
    c.startMinUtc = DUCK_SLEEP_START_MIN_UTC;
    c.wakeMinUtc = DUCK_SLEEP_WAKE_MIN_UTC;
    c.role = DUCK_SLEEP_ROLE == 0 ? Role::Leaf : Role::Relay;
    c.wakeJitterS = DUCK_SLEEP_WAKE_JITTER_S;
    c.maxChunkS = DUCK_SLEEP_MAX_CHUNK_S;
    c.survivalEnabled = DUCK_SURVIVAL_ENABLED == 1;
    c.survivalSleepMv = DUCK_SURVIVAL_SLEEP_MV;
    c.survivalWakeMv = DUCK_SURVIVAL_WAKE_MV;
    c.survivalRecheckMin = DUCK_SURVIVAL_RECHECK_MIN;
    c.buildEpoch = static_cast<std::int64_t>(DUCK_BUILD_EPOCH);
    return c;
}

/// The CDP topic the sleepstatus payload goes out on.
constexpr std::uint8_t kStatusTopic = DUCK_SLEEP_STATUS_TOPIC;

}  // namespace duckSleep
