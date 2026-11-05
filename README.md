# QuackPack-DuckSleep

[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![ClusterDuck Protocol](https://img.shields.io/badge/ClusterDuck-Protocol-yellow.svg)](https://github.com/ClusterDuck-Protocol/ClusterDuck-Protocol)
[![CI](https://github.com/Project-Owl/QuackPack-DuckSleep/actions/workflows/ci.yml/badge.svg)](https://github.com/Project-Owl/QuackPack-DuckSleep/actions/workflows/ci.yml)

Opt-in deep sleep for ClusterDuck Protocol (CDP) MamaDucks on the ESP32.

## Overview

This QuackPack puts a MamaDuck into ESP32 deep sleep in two cases:

- **Scheduled sleep** in one daily UTC window, shared by the whole mesh.
- **Battery survival hold**: sleep while the cell is low, with or without a clock.

It also:

- goes straight back to sleep after an early timer wake or a chunk boundary, before any rail or radio is on;
- reports each sleep on the mesh with a `sleeping` and an `awake` status (the `sleepstatus` payload);
- has an optional AXP2101 helper for T-Beam v1.2 class boards.

It is **off by default**. With both build switches at 0, every call compiles to an empty inline function and the firmware is byte-for-byte the same size as without the library.

The library does not include any CDP or DMS header. Everything CDP-specific (transmit queue, radio, sending, clock check) comes in through a `Hooks` struct of callbacks set by the sketch.

Log lines go to `Serial` with the prefix `[SLEEP]`. A silent re-sleep logs one line through the ROM printer, because `Serial` is not started yet.

### Who may sleep

| Duck | Sleep |
|---|---|
| PapaDuck | Never. It is the gateway, the NTP and `cmd_time` source and the command entry point. |
| DetectorDuck | Never. |
| MamaDuck (relay) | Only in one mesh-wide window per PapaDuck or deployment. `DUCK_SLEEP_ROLE=1` (the default) makes it fall asleep 10 minutes after the leaves and wake 10 minutes before them. |
| Leaf (a duck nobody relays through) | `DUCK_SLEEP_ROLE=0`: sleeps the configured window exactly. |

A sleeping MamaDuck does not relay, and CDP 5.5 has no store-and-forward. Never give a relay a window of its own. There is no duty cycling: an awake period under 15 minutes never sends CDP health v4 or signal reports.

Scheduled sleep needs a clock. Without one (no PapaDuck `cmd_time`, no NTP, no GPS), the duck stays awake and only the survival hold protects the battery.

## What is a QuackPack?

A **QuackPack** is a modular library that extends Duck firmware with application-specific functionality. QuackPacks are **not standalone projects**: they are included in a Duck firmware project and configured from its sketch.

## Hardware

### Supported boards

- **Primary**: LilyGO T-Beam v1.2 (AXP2101 PMU). The example builds for `ttgo-t-beam`, and the AXP2101 helper is written for this board.
- **Also**: any ESP32 board with the Arduino framework. DuckSleep uses ESP32 deep sleep and RTC memory; with sleep or survival enabled, the build fails on any other chip.

### Wiring

DuckSleep uses no pins of its own. It needs only the deep-sleep timer and RTC memory. On a T-Beam v1.2 the AXP2101 helper talks to the PMU over I2C and switches these rails:

| Signal | T-Beam v1.2 | Notes |
|---|---|---|
| I2C SDA | GPIO 21 | AXP2101 at address 0x34 |
| I2C SCL | GPIO 22 | |
| LoRa power | AXP2101 ALDO2 | 3.3 V; `railsOff()` cuts it |
| GPS power | AXP2101 ALDO3 | 3.3 V; `railsOff()` cuts it |
| Auxiliary power | AXP2101 ALDO4 | 3.3 V; `railsOff()` cuts it |
| ESP32 power | AXP2101 DCDC1 | never touched |

The sketch passes the I2C pins to `axp2101::begin()`. 21 and 22 are the T-Beam pins the example and the Quick start sketch use.

### AXP2101 helper (T-Beam v1.2)

`DuckSleepAxp2101.h` (T-Beam v1.2 class boards) is header-only templates over the XPowersLib PMU type, in namespace `duckSleep::axp2101`. The sketch includes XPowersLib first:

```cpp
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <DuckSleepAxp2101.h>
XPowersPMU PMU;
```

| Function | What |
|---|---|
| `bool begin(pmu, wire, sda, scl, attempts = 3)` | Starts the PMU at 0x34, retrying with an I2C bus restart (20, 40 ms waits). Turns on battery detection and the battery ADC. |
| `void railsOn(pmu)` | ALDO2 (LoRa), ALDO3 (GPS), ALDO4 (aux) on at 3.3 V. Call before CDP setup on every boot. |
| `void railsOff(pmu)` | ALDO3, ALDO4, ALDO2 off. For `powerGate(false)`. DCDC1 (the ESP32) is never touched. |
| `int32_t batteryMv(pmu)` | Cell voltage in mV, or -1 with no cell or an invalid sample. |

The AXP2101 keeps its rail state across ESP32 resets and deep sleep, so rails cut before a sleep stay off through silent re-sleeps.

XPowersLib 0.3.x binds the I2C address on the first `begin()` only; a second `begin()` with 0x35 (as older sketches do after a failure) just retries 0x34.

### Load switches

A load switch held off through deep sleep (the Phoenix Teensy pattern) goes in a power gate:

```cpp
duckSleep::addPowerGate([](bool on) {
    if (on) {                       // after a DuckSleep wake: release the hold
        gpio_hold_dis(LOAD_EN);
        gpio_deep_sleep_hold_dis();
        return;
    }
    digitalWrite(LOAD_EN, LOW);     // before deep sleep: off, and keep it off
    gpio_hold_en(LOAD_EN);
    gpio_deep_sleep_hold_en();
});
```

## Installation

### Requirements

- [PlatformIO](https://platformio.org/)
- An ESP32 with the Arduino framework
- C++17 or later (CDP projects build with `gnu++2a`)
- ArduinoJson 7 (`bblanchon/ArduinoJson@^7.0.3`, a dependency in `library.json`; CDP already uses it)
- `lewisxhe/XPowersLib` 0.3.x, only if the sketch uses `DuckSleepAxp2101.h`. It is not a library dependency.
- For the `txIdle`, `prepareRadioForSleep` and `timeValid` hooks: CDP `release/5.5` (see [Compatibility](#compatibility))

### Pinning it in a project (DMS, CDP or your own)

Add the QuackPack to your project's `lib_deps`. Pin it by **commit SHA** and name the tag in a comment, the same way the project pins CDP. A tag can be moved; a SHA cannot. Name the dependency (`QuackPack-DuckSleep = ...`) so PlatformIO treats every reference as one package.

```ini
lib_deps =
    ; your project owns the CDP pin (release/5.5 for the CDP 5.5 hooks)
    ClusterDuck-Protocol = https://github.com/Project-Owl/ClusterDuck-Protocol.git#<CDP commit SHA>
    ; QuackPack-DuckSleep v0.1.0
    QuackPack-DuckSleep = https://github.com/Project-Owl/QuackPack-DuckSleep.git#001c45cd2a16ed118bf793800cab5e923b47fe43
    lewisxhe/XPowersLib@^0.3.0          ; only for the optional AXP2101 helper
build_flags =
    -DDUCK_SLEEP_ENABLED=1
    -DDUCK_SLEEP_START_MIN_UTC=1320     ; 22:00 UTC
    -DDUCK_SLEEP_WAKE_MIN_UTC=360       ; 06:00 UTC
    -DDUCK_SURVIVAL_ENABLED=1
    -DDUCK_BUILD_EPOCH=1793750400       ; e.g. $(date +%s) at build time
```

`001c45cd` is the commit of tag `v0.1.0`. `git rev-list -n 1 v0.1.0` prints the commit of a tag.

The defines must reach the library sources, so set them in `build_flags` or `PLATFORMIO_BUILD_FLAGS`. A `#define` in the sketch does not reach `DuckSleep.cpp`. See [Build defines](#build-defines) for every define.

The library depends only on `bblanchon/ArduinoJson` (`^7.0.3`), which PlatformIO installs automatically. CDP already uses it.

**It has no ClusterDuck-Protocol dependency on purpose.** Everything CDP-specific comes in through hooks, so the library can be pinned next to any CDP version, and no URL dependency pulls in a second copy of CDP. Each project pins CDP itself.

## Quick start

A CDP 5.5 MamaDuck on a T-Beam v1.2 with the AXP2101 helper. The sketch makes the same calls whether sleep is built in or not:

```cpp
#include <string>
#include <vector>
#include <CDP.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <DuckSleep.h>
#include <DuckSleepAxp2101.h>

MamaDuck duck("MAMA0001");
auto timer = timer_create_default();
XPowersPMU PMU;
bool pmuOk = false;

static std::int32_t earlyBatteryMv() {
    return duckSleep::axp2101::begin(PMU, Wire, 21, 22, 1) ? duckSleep::axp2101::batteryMv(PMU) : -1;
}

void setup() {
    duckSleep::earlyBoot(earlyBatteryMv);  // first line: may go straight back to sleep

    Serial.begin(115200);
    pmuOk = duckSleep::axp2101::begin(PMU, Wire, 21, 22);
    if (pmuOk) {
        duckSleep::axp2101::railsOn(PMU);  // a previous sleep may have cut them
        delay(100);
    }
    if (duck.setupWithDefaults() != DUCK_ERR_NONE) return;

    duckSleep::Hooks hooks;
    hooks.txIdle = [] { return duck.txIdle(); };
    hooks.prepareRadioForSleep = [] { duck.prepareForSleep(); };
    hooks.sendStatus = [](std::uint8_t topic, const char* json) {
        return duck.sendData(topic, std::string(json)) == DUCK_ERR_NONE;
    };
    hooks.batteryMv = []() -> std::int32_t { return pmuOk ? duckSleep::axp2101::batteryMv(PMU) : -1; };
    hooks.timeValid = [] { return duck.hasValidTime(); };
    hooks.pmuAvailable = [] { return pmuOk; };
    hooks.powerGate = [](bool on) {
        if (!on && pmuOk) duckSleep::axp2101::railsOff(PMU);
    };
    const std::vector<uint8_t> id = duck.getDuckId();
    duckSleep::begin(hooks, duckSleep::deviceIdHash(id.data(), id.size()));

    // ...telemetry timers...
}

void loop() {
    duck.run();
    duckSleep::loop();
    if (duckSleep::sleepPending()) return;  // keep run() going, start nothing new
    timer.tick();
}
```

`Duck::txIdle()`, `Duck::prepareForSleep()` and `Duck::hasValidTime()` are new in CDP 5.5 (`release/5.5`; the early 5.5 commit 0f476934 does not have them). With a CDP that lacks them, leave `txIdle`, `prepareRadioForSleep` and `timeValid` unset: the timed fallback and the epoch checks are used.

This sketch was compiled against CDP `release/5.5` (espressif32 7.0.1, `ttgo-t-beam`) with sleep off and on. CI does not build it, because that needs CDP.

For scheduled sleep the duck needs a clock: build with `-DCDPCFG_REMOTE_TIME_SYNC=1` so CDP applies the PapaDuck's `cmd_time`, or set the clock from GPS.

[examples/BasicSleep](examples/BasicSleep/BasicSleep.ino) has the same wiring without CDP, with the CDP 5.5 call next to each stand-in.

## API

Include `<DuckSleep.h>`; everything is in namespace `duckSleep`.

```cpp
void earlyBoot(std::int32_t (*earlyBatteryMv)() = nullptr);
void begin(const Hooks& hooks, std::uint32_t idHash);
void loop();
bool sleepPending();
WakeCause lastWakeCause();
std::uint16_t lastSleepChunks();
bool addPowerGate(void (*gate)(bool on));

std::uint32_t deviceIdHash(const void* data, std::size_t length);
const char* wakeCauseName(WakeCause cause);
constexpr bool kBuiltIn;
```

| Call | When | What |
|---|---|---|
| `earlyBoot(reader)` | First line of `setup()` | After a deep-sleep timer wake: if the sleep is not over (early wake, chunk boundary, cell still low), go straight back to sleep with no rails, no radio, no CDP and no packets. Otherwise classify the boot and return. `reader` is optional and runs only on such a wake when the survival hold is built in; it must work before any rail is on (the AXP2101 helper does, over I2C). |
| `begin(hooks, idHash)` | After CDP setup | Copies the hooks, releases power gates after a DuckSleep wake, schedules the "awake" status. Calls `earlyBoot()` if the sketch did not. |
| `loop()` | Every `loop()` pass, after `duck.run()` | Checks the policy once a minute; runs the sleep entry sequence without blocking. Does not return when the duck goes to sleep. |
| `sleepPending()` | Before starting telemetry | True while a sleep is armed. Keep calling `duck.run()` so queued packets leave, but start nothing new. |
| `lastWakeCause()` | Boot payloads | `ColdBoot`, `Reset` (a sleep was armed, something else woke it), `ScheduleEnd`, `ClockLost`, `BatteryRecovered`, `BatteryCheck` (survival wake without an early reading). `wakeCauseName()` gives `cold`, `reset`, `schedule_end`, `clock_lost`, `battery_recovered`, `battery_check`. |
| `lastSleepChunks()` | Boot payloads | Deep sleeps in the sleep that just ended. More than `ceil(planned / DUCK_SLEEP_MAX_CHUNK_S)` means early timer wakes. |
| `addPowerGate(gate)` | Before `begin()` | Up to 4 extra gates, e.g. from another QuackPack. They run after `Hooks::powerGate`, in registration order. |
| `deviceIdHash(data, len)` | For `begin()` | FNV-1a of the device ID. |

Constants: `duckSleep::kBuiltIn` is true when DuckSleep is built in (`DUCK_SLEEP_ENABLED` or `DUCK_SURVIVAL_ENABLED` is 1). `duckSleep::kMaxExtraPowerGates` is 4.

The policy (`SleepPolicy.h`) and the payload builder (`SleepStatusMessage.h`) are plain headers with no Arduino dependency, usable and testable on their own.

### Hooks

Every hook is optional. Captureless lambdas convert to these function pointers.

| Hook | CDP 5.5 | Without it |
|---|---|---|
| `bool txIdle()` | `duck.txIdle()` | Fixed waits: 4 s before the status packet, 15 s after it (the Phoenix timings). |
| `void prepareRadioForSleep()` | `duck.prepareForSleep()` | Skipped; the radio rail cut (if any) does the job. |
| `bool sendStatus(uint8_t topic, const char* json)` | `duck.sendData(topic, std::string(json)) == DUCK_ERR_NONE` | No sleepstatus packets. |
| `int32_t batteryMv()` | e.g. `axp2101::batteryMv(PMU)` | The survival hold never starts (fail awake). |
| `bool timeValid()` | `duck.hasValidTime()` | Only the 2025 floor and `DUCK_BUILD_EPOCH`. |
| `bool pmuAvailable()` | your PMU init result | `error` never reported. |
| `void powerGate(bool on)` | e.g. `axp2101::railsOff(PMU)` | Nothing is switched off. |

`powerGate(false)` runs right before deep sleep, after `prepareRadioForSleep()`. `powerGate(true)` runs from `begin()` after a DuckSleep wake, to release pin holds; rails the radio needs must already be back on before CDP setup.

The clock is always the ESP32 system clock (`time()`): CDP's `cmd_time`, SNTP and GPS code set it, and it keeps running through deep sleep.

### The sleepstatus payload

Topic `DUCK_SLEEP_STATUS_TOPIC` (0x1B), one flat JSON object, at most 228 bytes (the longest possible is 160):

```json
{"state":"sleeping","reason":"sleep_window","planS":28710,"wakeAt":1793829600,"wake":"cold","mv":3912}
{"state":"awake","reason":"sleep_window","sleptS":28650,"chunks":8,"wake":"schedule_end","mv":3874}
{"state":"awake","reason":"battery_recovered","chunks":3,"wake":"battery_recovered","mv":null,"error":"pmu_unavailable"}
```

| Key | When | Meaning |
|---|---|---|
| `state` | always | `sleeping` or `awake` |
| `reason` | always | sleeping: `sleep_window`, `battery_low`, `battery_hold`. awake: `sleep_window`, `battery_recovered`, `interrupted`, `clock_lost` |
| `planS` | sleeping | seconds until the planned wake or the next battery check |
| `wakeAt` | sleeping, clock known | planned wake, UTC epoch |
| `sleptS` | awake, clock known at both ends | seconds asleep |
| `chunks` | awake | deep sleeps in that sleep, including silent re-sleeps |
| `wake` | always | `wakeCauseName()` of the current boot |
| `mv` | always | cell voltage in mV, or `null` |
| `error` | PMU not answering | `pmu_unavailable` |

A survival hold that continues after a boot without an early battery reading sends no new `sleeping` status each recheck.

### How a sleep runs

While awake, `loop()` asks the policy once a minute:

1. **Survival first.** With a plausible reading at or below `DUCK_SURVIVAL_SLEEP_MV` (twice in a row), hold for `DUCK_SURVIVAL_RECHECK_MIN`. Once in the hold, only `DUCK_SURVIVAL_WAKE_MV` or more ends it. No reading: stay awake.
2. **Schedule.** With a valid clock inside the effective window (after the relay guard), sleep until the window ends minus this duck's jitter. Never longer than the window, in chunks of at most `DUCK_SLEEP_MAX_CHUNK_S`.

Sleep entry, without blocking:

1. Armed: `sleepPending()` turns true; the sketch stops starting telemetry.
2. Wait until `txIdle()` has held for 0.5 s (at most 10 s), then send the `sleeping` status.
3. Wait at least 1 s, then until `txIdle()` holds again (at most 20 s).
4. Re-plan the chunk against the clock, write the retained state (RTC memory, with a magic word), `prepareRadioForSleep()`, power gates off, deep sleep.

On every timer wake, `earlyBoot()` decides from the retained state alone:

- Scheduled sleep with a minute or more left: back to sleep, silently.
- Scheduled sleep over, cell low: straight into the survival hold, silently.
- Survival hold, early reading still below the wake threshold: back to sleep.
- Survival hold recovered inside the window: straight into the scheduled sleep.
- Clock invalid, or the target further away than the window is long: wake (`clock_lost`). Any non-timer reset: wake (`reset`).
- Otherwise wake: rails on, CDP setup, and about 10 to 15 s after `begin()` the `awake` status.

### Build defines

Set these with `-D` in `build_flags`. All values are non-negative integers with a default and a `static_assert` range check: an out-of-range value fails the build.

| Define | Default | Range | Meaning |
|---|---|---|---|
| `DUCK_SLEEP_ENABLED` | `0` | 0, 1 | Scheduled sleep on. |
| `DUCK_SLEEP_START_MIN_UTC` | `0` | 0..1439 | Leaves fall asleep at this UTC minute of day. |
| `DUCK_SLEEP_WAKE_MIN_UTC` | `0` | 0..1439 | Leaves are awake again by this UTC minute. Equal to the start: no window. The window may cross midnight. |
| `DUCK_SLEEP_ROLE` | `1` | 0, 1 | 0 leaf, 1 relay (sleeps 10 min later, wakes 10 min earlier). A relay window of 20 minutes or less never sleeps. |
| `DUCK_SLEEP_WAKE_JITTER_S` | `90` | 0..600 | Wake up to this many seconds early, fixed per device ID, so the mesh does not wake in one burst. At most the relay guard, so relays still wake first. |
| `DUCK_SLEEP_MAX_CHUNK_S` | `3600` | 60..86400 | Longest single deep sleep. Each chunk boundary recalibrates the slow clock and can check the battery, without powering the radio. |
| `DUCK_SURVIVAL_ENABLED` | `0` | 0, 1 | Battery survival hold on. |
| `DUCK_SURVIVAL_SLEEP_MV` | `3400` | 3000..4100 | Enter the hold at or below this cell voltage. |
| `DUCK_SURVIVAL_WAKE_MV` | `3700` | 3100..4200, above the sleep value | Leave the hold only at or above this voltage. |
| `DUCK_SURVIVAL_RECHECK_MIN` | `30` | 5..720 | Minutes between battery checks in the hold (chunked like any sleep). |
| `DUCK_SLEEP_STATUS_TOPIC` | `0x1B` | 0x10..0xFF | CDP topic of the sleepstatus payload. 0x1B is the Owl `sleepstatus` topic the DMS decodes by name. |
| `DUCK_BUILD_EPOCH` | `0` | 0 or 2024..2100 | Build time, UTC epoch. A clock older than this is wrong and never triggers a schedule. |

Fixed behaviour, not configurable: a clock before 2025-01-01 is "no time"; no sleep shorter than 60 s; no scheduled sleep in the first 5 minutes after a cold boot or an unplanned reset (the survival hold is not delayed); battery readings outside 3000..4500 mV count as "no reading" and never cause sleep; entering the survival hold needs two low readings a minute apart.

## Building the example

```bash
pio run -e example_off              # DuckSleep compiled away (the default env)
pio run -e example_on               # 22:00-06:00 UTC window, relay, survival hold on
pio run -e example_on -t upload
pio device monitor
```

`platformio.ini` builds [examples/BasicSleep](examples/BasicSleep/BasicSleep.ino) the way a DMS or CDP MamaDuck project pulls the library in: espressif32 7.0.1, board `ttgo-t-beam`, `-std=gnu++2a`, the library through `symlink://./`, and `lewisxhe/XPowersLib@^0.3.0`. `example_on` also sets `DUCK_BUILD_EPOCH=1793750400`.

The example has no CDP. Its CDP calls are stand-ins, marked `CDP 5.5:` with the real call next to them. It sets the clock only when built with `-DEXAMPLE_FAKE_EPOCH=<epoch>`; a real duck gets it from the PapaDuck's `cmd_time`, NTP or GPS.

### Tests and CI

```sh
ARDUINOJSON_SRC=/path/to/ArduinoJson/src tests/host/run.sh
pio run -e example_off
pio run -e example_on
```

`tests/host/run.sh` builds with `clang++ -std=c++17 -Wall -Wextra -Wshadow -Werror -fsanitize=address,undefined` and checks:

- the policy: windows crossing midnight, relay offsets and their ordering against leaves, jitter bounds, never sleeping longer than the window, chunking, no clock, a clock older than the build, the install grace, survival hysteresis and debounce, and every early-boot path (cold boot, stale arm after a reset, early wake re-sleep including a simulated 3.6 % drift, clock lost, survival recheck and recovery, schedule-to-survival and survival-to-schedule hand-overs);
- the payload: exact output, nulls, the PMU flag, the 228-byte limit;
- `DuckSleep.h` with the defaults (all no-ops) and enabled;
- every out-of-range define fails to compile.

Without `ARDUINOJSON_SRC` it uses `.pio/libdeps/*/ArduinoJson/src` from a previous `pio run`.

`.github/workflows/ci.yml` runs the host tests (against ArduinoJson 7.4.3) and builds the example both ways on every push and pull request.

## Compatibility

- **ClusterDuck Protocol**: any version, through the hooks; there is no CDP dependency. The `txIdle`, `prepareRadioForSleep` and `timeValid` hooks map to calls new in CDP `release/5.5`, and the Quick start sketch was compiled against it.
- **Platform**: ESP32 only; the example and CI use espressif32 7.0.1 with board `ttgo-t-beam`
- **Language**: C++17 or later
- **ArduinoJson**: 7 (`^7.0.3`; the host tests run against 7.4.3)
- **XPowersLib**: 0.3.x, for the AXP2101 helper only
- **Development environment**: PlatformIO only (the Arduino IDE is not used or tested)

### Caveats

- **Wake storm.** A mesh-wide window wakes many ducks together. The jitter spreads wakes over `DUCK_SLEEP_WAKE_JITTER_S`; stagger the first telemetry after boot in the sketch too.
- **Slow-clock drift.** Long ESP32 deep sleeps woke up to 18.5 minutes early in an 8.5-hour sleep in the Phoenix soak. Chunking and the silent re-sleep make an early wake cost a few hundred milliseconds instead of a boot and a rejoin.
- **Unauthenticated time.** CDP `cmd_time` is not authenticated. A wrong clock can start or skip a sleep. `DUCK_BUILD_EPOCH`, the window cap and failing awake limit the damage; they do not prevent it.
- **Time zones.** The window is in UTC and does not follow daylight saving.
- **Every wake is a reboot** for CDP: routes, counters, health `boot` and `uptimeS` reset, and the reset reason is 8 (deep sleep). Dashboards should treat scheduled windows as expected, not as outages.
- **PMU failures.** If the PMU does not answer, rails are not cut and the sleep saves less; the status says `pmu_unavailable`.
- **Not covered:** the AXP2101 VBUS-sag latch-off, which needs a power cycle.

## Contributing

Open an issue or a pull request on [GitHub](https://github.com/Project-Owl/QuackPack-DuckSleep). Run the host tests and both example builds first (see [Tests and CI](#tests-and-ci)); CI runs the same checks on every pull request.

### Layout

```
src/DuckSleep.h            public API and Hooks (no Arduino headers)
src/DuckSleepConfig.h      defines, range checks, Config from defines
src/SleepPolicy.h          pure policy: decide(), decideBoot()
src/SleepStatusMessage.h   sleepstatus payload builder (ArduinoJson)
src/DuckSleep.cpp          ESP32 adapter (compiled only when enabled)
src/DuckSleepAxp2101.h     optional AXP2101 helper (needs XPowersLib)
examples/BasicSleep        T-Beam example without CDP
tests/host                 host tests
```

## Changelog

### 0.1.0

- Initial release: scheduled sleep in a daily UTC window with the relay guard and per-device wake jitter, the battery survival hold, chunked sleeps with a silent re-sleep after early timer wakes, the `sleepstatus` payload, the optional AXP2101 helper, host tests and CI.

## License

This project is licensed under the Apache 2.0 License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built on the [ClusterDuck Protocol](https://github.com/ClusterDuck-Protocol/ClusterDuck-Protocol)
- Created by [OWL Integrations](https://www.owlintegrations.com/)
- The fallback timings and the slow-clock drift figures come from the Phoenix firmware and its soak test.

## Resources

- [OWL Integrations](https://www.owlintegrations.com/)
- [OWL DMS](https://www.owlintegrations.com/) - Device Management System for mesh networking
- [ClusterDuck Protocol](https://github.com/ClusterDuck-Protocol/ClusterDuck-Protocol)
- [Discord Community](https://discord.gg/clusterduck)
- **DMS Support**: support@owlintegrations.com

## Support

For questions and support:
- Join the [ClusterDuck Protocol Discord](https://discord.gg/clusterduck)
- Open an [Issue](https://github.com/Project-Owl/QuackPack-DuckSleep/issues)

## Authors

- **Timo Wielink** - *Initial work* - [@TimoWielink](https://github.com/TimoWielink)
