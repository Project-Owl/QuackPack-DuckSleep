/**
 * @file DuckSleepAxp2101.h
 * @brief Optional AXP2101 helpers for T-Beam v1.2 class boards: PMU start-up
 * with retry, the rails DuckSleep cuts, and the cell voltage.
 *
 * Header-only templates over the PMU type, so DuckSleep itself does not depend
 * on XPowersLib. The sketch includes XPowersLib first:
 *
 *   #define XPOWERS_CHIP_AXP2101
 *   #include <XPowersLib.h>
 *   #include <DuckSleepAxp2101.h>
 *   XPowersPMU PMU;
 *
 * Rails on the T-Beam v1.2: ALDO2 LoRa, ALDO3 GPS, ALDO4 auxiliary (all 3.3 V).
 * DCDC1 feeds the ESP32 and is never touched. The AXP2101 keeps its rail
 * state across ESP32 deep sleep and resets, so rails cut before a sleep stay
 * off through silent re-sleeps, and the sketch must switch them back on
 * (railsOn()) before CDP setup on every boot.
 */
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include <cstdint>

namespace duckSleep {
namespace axp2101 {

constexpr std::uint8_t kAddress = 0x34;  ///< the AXP2101 has only this address
constexpr int kRailMillivolts = 3300;

/**
 * @brief Starts the PMU, retrying a few times with an I2C bus restart in between.
 *
 * XPowersLib 0.3.x binds the bus and address on the first begin() only; later
 * calls just re-read the chip ID. A retry therefore recovers a transient bus
 * fault, but trying another address (0x35) after a failure does nothing.
 * Also turns on battery detection and the battery voltage ADC.
 * Waits 20 ms, 40 ms, ... between attempts (60 ms in all with the default 3):
 * call it from setup() or earlyBoot(), not loop().
 *
 * @returns true when the PMU answered.
 */
template <class Pmu>
bool begin(Pmu& pmu, TwoWire& wire, int sda, int scl, std::uint8_t attempts = 3) {
    for (std::uint8_t i = 0; i < attempts; ++i) {
        if (i > 0) {
            wire.end();
            delay(20u * i);
            wire.begin(sda, scl);
        }
        if (pmu.begin(wire, kAddress, sda, scl)) {
            pmu.enableBattDetection();
            pmu.enableBattVoltageMeasure();
            return true;
        }
    }
    return false;
}

/// Switches on the LoRa, GPS and auxiliary rails at 3.3 V. Call before CDP setup.
template <class Pmu>
void railsOn(Pmu& pmu) {
    pmu.setALDO2Voltage(kRailMillivolts);
    pmu.setALDO3Voltage(kRailMillivolts);
    pmu.setALDO4Voltage(kRailMillivolts);
    pmu.enableALDO2();
    pmu.enableALDO3();
    pmu.enableALDO4();
}

/// Cuts the GPS, auxiliary and LoRa rails. For Hooks::powerGate(false), after
/// the radio has been put to sleep.
template <class Pmu>
void railsOff(Pmu& pmu) {
    pmu.disableALDO3();
    pmu.disableALDO4();
    pmu.disableALDO2();
}

/// Cell voltage in mV, or -1 when no cell is detected or the reading is not
/// a valid 14-bit sample.
template <class Pmu>
std::int32_t batteryMv(Pmu& pmu) {
    if (!pmu.isBatteryConnect()) return -1;
    const std::uint16_t mv = pmu.getBattVoltage();
    if (mv == 0 || mv > 0x3FFF) return -1;
    return static_cast<std::int32_t>(mv);
}

}  // namespace axp2101
}  // namespace duckSleep
