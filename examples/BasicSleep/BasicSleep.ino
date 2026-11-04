/**
 * BasicSleep: DuckSleep on a T-Beam v1.2 (AXP2101), without CDP.
 *
 * Shows the three calls every MamaDuck sketch makes, the AXP2101 helper, and
 * how each hook is wired. The CDP calls are stand-ins, marked "CDP 5.5:" with
 * the real call next to them; the README has the full MamaDuck version.
 *
 * Build it with sleep off (the default) and on:
 *   pio run -e example_off
 *   pio run -e example_on     (22:00-06:00 UTC window, survival hold on)
 *
 * With sleep off every duckSleep:: call compiles to nothing.
 */
#include <Arduino.h>
#include <Wire.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <DuckSleep.h>
#include <DuckSleepAxp2101.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

namespace {

constexpr int kI2cSda = 21;
constexpr int kI2cScl = 22;
constexpr char kDeviceId[] = "MAMA0001";
constexpr std::uint32_t kTelemetryIntervalMs = 30000;

XPowersPMU PMU;
bool pmuOk = false;
std::uint32_t lastTelemetryMs = 0;
std::uint32_t telemetryCount = 0;

// Stand-in for the CDP transmit queue: "sent" packets leave after 300 ms.
std::uint32_t lastSendMs = 0;
bool radioBusy() { return std::uint32_t(millis() - lastSendMs) < 300; }

bool sendOnMesh(std::uint8_t topic, const char* json) {
    // CDP 5.5: return duck.sendData(topic, std::string(json)) == DUCK_ERR_NONE;
    lastSendMs = millis();
    Serial.printf("[MESH] topic 0x%02X: %s\n", topic, json);
    return true;
}

// Runs inside earlyBoot(), before any rail is on. Only I2C to the PMU.
std::int32_t earlyBatteryMv() {
    return duckSleep::axp2101::begin(PMU, Wire, kI2cSda, kI2cScl, 1) ? duckSleep::axp2101::batteryMv(PMU) : -1;
}

void setClockForDemo() {
    // A real duck gets its clock from the PapaDuck's cmd_time, NTP or GPS.
    // Here the clock is set only when a build passes -DEXAMPLE_FAKE_EPOCH.
#ifdef EXAMPLE_FAKE_EPOCH
    if (std::time(nullptr) < 1735689600) {
        timeval tv = {static_cast<time_t>(EXAMPLE_FAKE_EPOCH), 0};
        settimeofday(&tv, nullptr);
    }
#endif
}

}  // namespace

void setup() {
    // First line: after an early timer wake this goes straight back to sleep.
    duckSleep::earlyBoot(earlyBatteryMv);

    Serial.begin(115200);
    Serial.printf("[BOOT] DuckSleep %s, wake cause %s\n", duckSleep::kBuiltIn ? "built in" : "off",
                  duckSleep::wakeCauseName(duckSleep::lastWakeCause()));

    // Rails back on before the radio is touched: a previous sleep cut them.
    pmuOk = duckSleep::axp2101::begin(PMU, Wire, kI2cSda, kI2cScl);
    if (pmuOk) {
        duckSleep::axp2101::railsOn(PMU);
        delay(100);  // let the LoRa rail settle
    } else {
        Serial.println("[BOOT] PMU not answering: rails stay as they are");
    }

    // CDP 5.5: duck.setupWithDefaults();
    setClockForDemo();

    duckSleep::Hooks hooks;
    hooks.txIdle = [] { return !radioBusy(); };            // CDP 5.5: [] { return duck.txIdle(); }
    hooks.prepareRadioForSleep = [] {};                    // CDP 5.5: [] { duck.prepareForSleep(); }
    hooks.sendStatus = sendOnMesh;
    hooks.batteryMv = []() -> std::int32_t { return pmuOk ? duckSleep::axp2101::batteryMv(PMU) : -1; };
    hooks.timeValid = [] { return std::time(nullptr) >= 1735689600; };  // CDP 5.5: duck.hasValidTime()
    hooks.pmuAvailable = [] { return pmuOk; };
    hooks.powerGate = [](bool on) {
        if (!on && pmuOk) duckSleep::axp2101::railsOff(PMU);
    };
    // CDP 5.5: auto id = duck.getDuckId(); deviceIdHash(id.data(), id.size())
    duckSleep::begin(hooks, duckSleep::deviceIdHash(kDeviceId, sizeof(kDeviceId) - 1));
}

void loop() {
    // CDP 5.5: duck.run();
    duckSleep::loop();
    if (duckSleep::sleepPending()) return;  // no new telemetry while a sleep is armed

    if (std::uint32_t(millis() - lastTelemetryMs) >= kTelemetryIntervalMs) {
        lastTelemetryMs = millis();
        char json[64];
        std::snprintf(json, sizeof(json), "{\"c\":%lu,\"mv\":%ld}", static_cast<unsigned long>(++telemetryCount),
                      static_cast<long>(pmuOk ? duckSleep::axp2101::batteryMv(PMU) : -1));
        sendOnMesh(0x18, json);  // CDP 5.5: topics::bat
    }
}
