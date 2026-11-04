// Compiles the public header on the host. With the defaults every call must be
// an inline no-op; with DUCK_SLEEP_ENABLED=1 the declarations and the define
// mapping must still compile without any Arduino header (run.sh then only
// compiles, since the adapter itself is ESP32-only).
#include "DuckSleep.h"

#include "check.h"

namespace {
std::uint8_t lastTopic = 0;
bool sendStub(std::uint8_t topic, const char*) {
    lastTopic = topic;
    return true;
}
}  // namespace

int main() {
    duckSleep::Hooks hooks;
    hooks.txIdle = [] { return true; };
    hooks.prepareRadioForSleep = [] {};
    hooks.sendStatus = sendStub;
    hooks.batteryMv = []() -> std::int32_t { return 3900; };
    hooks.timeValid = [] { return true; };
    hooks.pmuAvailable = [] { return true; };
    hooks.powerGate = [](bool) {};

    const char id[] = "MAMA0001";
    duckSleep::earlyBoot();
    duckSleep::earlyBoot([]() -> std::int32_t { return -1; });
    duckSleep::begin(hooks, duckSleep::deviceIdHash(id, 8));
    duckSleep::loop();
    const bool pending = duckSleep::sleepPending();
    const duckSleep::WakeCause cause = duckSleep::lastWakeCause();
    const std::uint16_t chunks = duckSleep::lastSleepChunks();
    const bool added = duckSleep::addPowerGate([](bool) {});

    const duckSleep::Config c = duckSleep::configFromDefines();
    CHECK_EQ(c.startMinUtc, DUCK_SLEEP_START_MIN_UTC);
    CHECK_EQ(c.wakeMinUtc, DUCK_SLEEP_WAKE_MIN_UTC);
    CHECK_EQ(static_cast<int>(c.role), DUCK_SLEEP_ROLE);
    CHECK_EQ(c.wakeJitterS, DUCK_SLEEP_WAKE_JITTER_S);
    CHECK_EQ(c.maxChunkS, DUCK_SLEEP_MAX_CHUNK_S);
    CHECK_EQ(c.survivalSleepMv, DUCK_SURVIVAL_SLEEP_MV);
    CHECK_EQ(c.survivalWakeMv, DUCK_SURVIVAL_WAKE_MV);
    CHECK_EQ(c.survivalRecheckMin, DUCK_SURVIVAL_RECHECK_MIN);
    CHECK_EQ(c.buildEpoch, DUCK_BUILD_EPOCH);
    CHECK_EQ(duckSleep::kStatusTopic, DUCK_SLEEP_STATUS_TOPIC);

#if DUCK_SLEEP_ACTIVE
    static_assert(duckSleep::kBuiltIn, "built in when enabled");
    (void)pending;
    (void)cause;
    (void)chunks;
    (void)added;
#else
    static_assert(!duckSleep::kBuiltIn, "compiled away by default");
    CHECK(!c.scheduleEnabled);
    CHECK(!c.survivalEnabled);
    CHECK(!pending);
    CHECK(cause == duckSleep::WakeCause::ColdBoot);
    CHECK_EQ(chunks, 0);
    CHECK(!added);
    CHECK_EQ(c.startMinUtc, 0);
    CHECK_EQ(c.wakeMinUtc, 0);
    CHECK(c.role == duckSleep::Role::Relay);
    CHECK_EQ(c.wakeJitterS, 90);
    CHECK_EQ(c.maxChunkS, 3600);
    CHECK_EQ(c.survivalSleepMv, 3400);
    CHECK_EQ(c.survivalWakeMv, 3700);
    CHECK_EQ(c.survivalRecheckMin, 30);
    CHECK_EQ(duckSleep::kStatusTopic, 0x1B);
    CHECK_EQ(c.buildEpoch, 0);
#endif
    (void)lastTopic;
    return checkSummary(duckSleep::kBuiltIn ? "DuckSleep.h (enabled)" : "DuckSleep.h (defaults)");
}
