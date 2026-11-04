// Host tests for SleepStatusMessage.h: exact payloads, nulls, the PMU flag and
// the 228-byte CDP payload limit.
#include "SleepStatusMessage.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "check.h"

using namespace duckSleep;

namespace {

void testSleeping() {
    StatusFields f;
    f.sleeping = true;
    f.reason = reasonName(Reason::SleepWindow);
    f.wake = WakeCause::ColdBoot;
    f.plannedS = 28710;
    f.wakeAtEpoch = 1793829600;
    f.batteryMv = 3912;
    f.pmu = PmuState::Ok;
    char out[kMaxPayload + 1];
    const std::size_t n = sleepStatusJson(f, out, sizeof(out));
    CHECK_STREQ(out, "{\"state\":\"sleeping\",\"reason\":\"sleep_window\",\"planS\":28710,"
                     "\"wakeAt\":1793829600,\"wake\":\"cold\",\"mv\":3912}");
    CHECK_EQ(n, std::strlen(out));

    // Unknown clock: no wakeAt. Survival hold reason.
    f.reason = reasonName(Reason::BatteryLow);
    f.wakeAtEpoch = 0;
    f.plannedS = 1800;
    f.batteryMv = 3390;
    sleepStatusJson(f, out, sizeof(out));
    CHECK_STREQ(out, "{\"state\":\"sleeping\",\"reason\":\"battery_low\",\"planS\":1800,"
                     "\"wake\":\"cold\",\"mv\":3390}");
}

void testAwake() {
    StatusFields f;
    f.sleeping = false;
    f.wake = WakeCause::ScheduleEnd;
    f.reason = awakeReasonName(f.wake);
    f.sleptS = 28650;
    f.chunks = 8;
    f.batteryMv = 3874;
    char out[kMaxPayload + 1];
    sleepStatusJson(f, out, sizeof(out));
    CHECK_STREQ(out, "{\"state\":\"awake\",\"reason\":\"sleep_window\",\"sleptS\":28650,"
                     "\"chunks\":8,\"wake\":\"schedule_end\",\"mv\":3874}");

    // Sleep length unknown, battery unknown, PMU not answering.
    f.wake = WakeCause::BatteryRecovered;
    f.reason = awakeReasonName(f.wake);
    f.sleptS = -1;
    f.chunks = 3;
    f.batteryMv = 0;
    f.pmu = PmuState::Unavailable;
    sleepStatusJson(f, out, sizeof(out));
    CHECK_STREQ(out, "{\"state\":\"awake\",\"reason\":\"battery_recovered\",\"chunks\":3,"
                     "\"wake\":\"battery_recovered\",\"mv\":null,\"error\":\"pmu_unavailable\"}");

    CHECK_STREQ(awakeReasonName(WakeCause::BatteryCheck), "battery_recovered");
    CHECK_STREQ(awakeReasonName(WakeCause::Reset), "interrupted");
    CHECK_STREQ(awakeReasonName(WakeCause::ClockLost), "clock_lost");
    CHECK_STREQ(awakeReasonName(WakeCause::ColdBoot), "cold_boot");
}

void testBatteryNull() {
    StatusFields f;
    f.sleeping = false;
    char out[kMaxPayload + 1];
    const std::int32_t bad[] = {0, -1, 2999, 4501};
    for (const std::int32_t mv : bad) {
        f.batteryMv = mv;
        CHECK(sleepStatusJson(f, out, sizeof(out)) > 0);
        CHECK(std::strstr(out, "\"mv\":null") != nullptr);
    }
    f.pmu = PmuState::Ok;
    sleepStatusJson(f, out, sizeof(out));
    CHECK(std::strstr(out, "error") == nullptr);
    f.pmu = PmuState::Unknown;
    sleepStatusJson(f, out, sizeof(out));
    CHECK(std::strstr(out, "error") == nullptr);
}

void testBounds() {
    // Every field at its longest still fits the CDP payload.
    StatusFields f;
    f.sleeping = true;
    f.reason = awakeReasonName(WakeCause::BatteryRecovered);
    f.wake = WakeCause::BatteryRecovered;
    f.plannedS = std::numeric_limits<std::uint32_t>::max();
    f.wakeAtEpoch = std::numeric_limits<std::int64_t>::max();
    f.batteryMv = 4500;
    f.pmu = PmuState::Unavailable;
    char out[kMaxPayload + 1];
    std::size_t n = sleepStatusJson(f, out, sizeof(out));
    CHECK(n > 0);
    CHECK(n <= kMaxPayload);
    std::size_t longest = n;

    f.sleeping = false;
    f.sleptS = std::numeric_limits<std::int64_t>::max();
    f.chunks = 0xFFFF;
    n = sleepStatusJson(f, out, sizeof(out));
    CHECK(n > 0);
    CHECK(n <= kMaxPayload);
    if (n > longest) longest = n;
    std::printf("longest sleepstatus payload: %zu of %zu bytes\n", longest, kMaxPayload);

    // Too small a buffer: nothing written, empty string.
    char small[32];
    std::memset(small, 'x', sizeof(small));
    CHECK_EQ(sleepStatusJson(f, small, sizeof(small)), 0);
    CHECK_EQ(small[0], '\0');
    // Exactly enough room.
    char exact[kMaxPayload + 1];
    const std::size_t need = sleepStatusJson(f, exact, sizeof(exact));
    char tight[kMaxPayload + 1];
    CHECK_EQ(sleepStatusJson(f, tight, need + 1), need);
    CHECK_EQ(sleepStatusJson(f, tight, need), 0);
    CHECK_EQ(sleepStatusJson(f, nullptr, 100), 0);
    CHECK_EQ(sleepStatusJson(f, tight, 0), 0);

    // A runaway reason string is refused, not truncated.
    std::string longReason(300, 'r');
    f.reason = longReason.c_str();
    CHECK_EQ(sleepStatusJson(f, out, sizeof(out)), 0);
    f.reason = nullptr;
    CHECK(sleepStatusJson(f, out, sizeof(out)) > 0);
    CHECK(std::strstr(out, "\"reason\":\"none\"") != nullptr);
}

}  // namespace

int main() {
    testSleeping();
    testAwake();
    testBatteryNull();
    testBounds();
    return checkSummary("SleepStatusMessage");
}
