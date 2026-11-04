// Host tests for SleepPolicy.h: windows, relay guard, jitter, chunking, clock
// validity, survival hysteresis and the early-boot re-sleep decisions.
#include "SleepPolicy.h"

#include <cstdint>

#include "check.h"

using namespace duckSleep;

namespace {

// 2026-11-04T00:00:00Z
constexpr std::int64_t kDay = 1793750400;

constexpr std::int64_t at(int day, int hour, int minute, int second = 0) {
    return kDay + day * 86400LL + hour * 3600LL + minute * 60LL + second;
}

Config night(Role role, std::uint16_t jitter = 0) {
    Config c;
    c.scheduleEnabled = true;
    c.startMinUtc = 22 * 60;  // 22:00
    c.wakeMinUtc = 6 * 60;    // 06:00, crosses midnight
    c.role = role;
    c.wakeJitterS = jitter;
    c.maxChunkS = 3600;
    return c;
}

Config survival(bool schedule = false) {
    Config c = night(Role::Leaf);
    c.scheduleEnabled = schedule;
    c.survivalEnabled = true;
    c.survivalSleepMv = 3400;
    c.survivalWakeMv = 3700;
    c.survivalRecheckMin = 30;
    return c;
}

Decision decideAt(const Config& c, std::int64_t epoch, std::uint32_t idHash = 0,
                  std::int32_t mv = 0, bool hold = false) {
    AwakeInput in;
    in.utcEpoch = epoch;
    in.idHash = idHash;
    in.batteryMv = mv;
    in.holdActive = hold;
    return decide(c, in);
}

// Arms the retained state the way the adapter does when a sleep starts.
Retained armedAt(const Config& c, std::int64_t epoch, std::uint32_t idHash = 0) {
    const Decision d = decideAt(c, epoch, idHash);
    Retained r;
    armRetained(r, d, epoch, idHash, false);
    return r;
}

BootDecision bootAt(const Config& c, Retained& r, std::int64_t epoch, std::int32_t mv = 0,
                    bool timer = true) {
    BootInput in;
    in.timerWake = timer;
    in.utcEpoch = epoch;
    in.batteryMv = mv;
    return decideBoot(c, r, in);
}

void testHelpers() {
    CHECK_EQ(secondOfDay(kDay), 0);
    CHECK_EQ(secondOfDay(at(0, 23, 59, 59)), 86399);
    CHECK_EQ(hashId(nullptr, 0), 2166136261u);
    CHECK_EQ(hashId("a", 1), 0xe40c292cu);  // FNV-1a test vector
    const char id[] = "MAMA0001";
    CHECK_EQ(hashId(id, 8), hashId(id, 8));
    CHECK(hashId(id, 8) != hashId("MAMA0002", 8));
    CHECK(!batteryPlausible(0));
    CHECK(!batteryPlausible(-1));
    CHECK(!batteryPlausible(2999));
    CHECK(batteryPlausible(3000));
    CHECK(batteryPlausible(4500));
    CHECK(!batteryPlausible(4501));
    for (int c = 0; c <= 5; ++c) {
        CHECK(std::strcmp(wakeCauseName(static_cast<WakeCause>(c)), "unknown") != 0);
    }
    for (int r = 0; r <= 9; ++r) {
        CHECK(std::strcmp(reasonName(static_cast<Reason>(r)), "unknown") != 0);
    }
}

void testWindows() {
    Config c = night(Role::Leaf);
    Window w = effectiveWindow(c);
    CHECK(w.valid);
    CHECK_EQ(w.startMin, 1320);
    CHECK_EQ(w.wakeMin, 360);
    CHECK_EQ(windowLengthS(w), 8 * 3600);

    c.role = Role::Relay;
    w = effectiveWindow(c);
    CHECK(w.valid);
    CHECK_EQ(w.startMin, 1330);
    CHECK_EQ(w.wakeMin, 350);
    CHECK_EQ(windowLengthS(w), 8 * 3600 - 20 * 60);

    // The relay guard itself can cross midnight.
    c.startMinUtc = 1435;
    c.wakeMinUtc = 30;
    w = effectiveWindow(c);
    CHECK(w.valid);
    CHECK_EQ(w.startMin, 5);
    CHECK_EQ(w.wakeMin, 20);

    // A relay never sleeps in a window that is not longer than both guards.
    c.startMinUtc = 1320;
    c.wakeMinUtc = 1340;
    CHECK(!effectiveWindow(c).valid);
    c.wakeMinUtc = 1341;
    CHECK(effectiveWindow(c).valid);
    c.role = Role::Leaf;
    c.wakeMinUtc = 1340;
    CHECK(effectiveWindow(c).valid);

    // start == wake: no window. Disabled: no window.
    c.wakeMinUtc = c.startMinUtc;
    CHECK(!effectiveWindow(c).valid);
    c = night(Role::Leaf);
    c.scheduleEnabled = false;
    CHECK(!effectiveWindow(c).valid);
    // Out-of-range minutes (not reachable through the defines) fail safe.
    c = night(Role::Leaf);
    c.startMinUtc = 1440;
    CHECK(!effectiveWindow(c).valid);
}

void testScheduleCrossingMidnight() {
    const Config c = night(Role::Leaf);

    Decision d = decideAt(c, at(0, 21, 59, 59));
    CHECK(d.action == Action::StayAwake);
    CHECK(d.reason == Reason::OutsideWindow);

    d = decideAt(c, at(0, 22, 0, 0));
    CHECK(d.action == Action::Sleep);
    CHECK(d.reason == Reason::SleepWindow);
    CHECK_EQ(d.totalS, 8 * 3600);
    CHECK_EQ(d.sleepS, 3600);
    CHECK_EQ(d.wakeAtEpoch, at(1, 6, 0, 0));

    d = decideAt(c, at(0, 23, 30, 0));
    CHECK_EQ(d.totalS, 6 * 3600 + 1800);
    CHECK_EQ(d.wakeAtEpoch, at(1, 6, 0, 0));

    d = decideAt(c, at(1, 0, 0, 0));
    CHECK(d.action == Action::Sleep);
    CHECK_EQ(d.totalS, 6 * 3600);

    d = decideAt(c, at(1, 5, 59, 0));
    CHECK(d.action == Action::Sleep);
    CHECK_EQ(d.totalS, 60);
    CHECK_EQ(d.sleepS, 60);

    d = decideAt(c, at(1, 5, 59, 1));
    CHECK(d.action == Action::StayAwake);
    CHECK(d.reason == Reason::WindowEnding);

    d = decideAt(c, at(1, 6, 0, 0));
    CHECK(d.action == Action::StayAwake);
    CHECK(d.reason == Reason::OutsideWindow);

    // A window that does not cross midnight.
    Config day = c;
    day.startMinUtc = 60;
    day.wakeMinUtc = 120;
    CHECK(decideAt(day, at(0, 0, 59, 59)).action == Action::StayAwake);
    d = decideAt(day, at(0, 1, 0, 0));
    CHECK(d.action == Action::Sleep);
    CHECK_EQ(d.totalS, 3600);
    CHECK(decideAt(day, at(0, 2, 0, 0)).reason == Reason::OutsideWindow);
}

void testRelayOffsets() {
    const Config relay = night(Role::Relay);
    CHECK(decideAt(relay, at(0, 22, 9, 59)).reason == Reason::OutsideWindow);
    Decision d = decideAt(relay, at(0, 22, 10, 0));
    CHECK(d.action == Action::Sleep);
    CHECK_EQ(d.totalS, 7 * 3600 + 40 * 60);
    CHECK_EQ(d.wakeAtEpoch, at(1, 5, 50, 0));
    CHECK(decideAt(relay, at(1, 5, 49, 0)).action == Action::Sleep);
    CHECK(decideAt(relay, at(1, 5, 50, 0)).reason == Reason::OutsideWindow);

    // With jitter, every relay is awake before any leaf wakes, by at least the
    // guard minus the jitter.
    const Config relayJ = night(Role::Relay, 90);
    const Config leafJ = night(Role::Leaf, 90);
    std::int64_t latestRelay = 0;
    std::int64_t earliestLeaf = at(9, 0, 0);
    for (std::uint32_t i = 0; i < 2000; ++i) {
        const std::uint32_t h = hashId(&i, sizeof(i));
        const Decision r = decideAt(relayJ, at(0, 23, 0, 0), h);
        const Decision l = decideAt(leafJ, at(0, 23, 0, 0), h);
        CHECK(r.action == Action::Sleep);
        CHECK(l.action == Action::Sleep);
        if (r.wakeAtEpoch > latestRelay) latestRelay = r.wakeAtEpoch;
        if (l.wakeAtEpoch < earliestLeaf) earliestLeaf = l.wakeAtEpoch;
    }
    CHECK(latestRelay <= at(1, 5, 50, 0));
    CHECK(earliestLeaf >= at(1, 5, 58, 30));
    CHECK(earliestLeaf - latestRelay >= 600 - 90);
}

void testJitterBounds() {
    CHECK_EQ(wakeJitterS(12345, 0), 0);
    std::uint32_t lo = 1000;
    std::uint32_t hi = 0;
    const Config c = night(Role::Leaf, 90);
    for (std::uint32_t i = 0; i < 5000; ++i) {
        const std::uint32_t h = hashId(&i, sizeof(i));
        const std::uint32_t j = wakeJitterS(h, 90);
        CHECK(j <= 90);
        CHECK_EQ(j, wakeJitterS(h, 90));  // fixed per device
        if (j < lo) lo = j;
        if (j > hi) hi = j;
        const Decision d = decideAt(c, at(0, 22, 0, 0), h);
        CHECK(d.wakeAtEpoch <= at(1, 6, 0, 0));
        CHECK(d.wakeAtEpoch >= at(1, 6, 0, 0) - 90);
        CHECK(wakeJitterS(h, 600) <= 600);
    }
    // The jitter actually spreads the mesh over the whole range.
    CHECK(lo <= 2);
    CHECK(hi >= 88);

    // Near the end of the window the jitter decides who stays awake.
    std::uint32_t idWith60 = 0;
    for (std::uint32_t i = 0; i < 100000; ++i) {
        if (wakeJitterS(hashId(&i, sizeof(i)), 90) == 60) {
            idWith60 = hashId(&i, sizeof(i));
            break;
        }
    }
    CHECK_EQ(wakeJitterS(idWith60, 90), 60);
    CHECK(decideAt(c, at(1, 5, 58, 0), idWith60).action == Action::Sleep);       // 120 - 60 = 60 s
    CHECK(decideAt(c, at(1, 5, 58, 1), idWith60).reason == Reason::WindowEnding);
}

void testNeverLongerThanWindow() {
    const Role roles[] = {Role::Leaf, Role::Relay};
    const std::uint16_t windows[][2] = {{1320, 360}, {60, 120}, {0, 1439}, {1439, 0}, {700, 699}, {1430, 15}};
    for (const Role role : roles) {
        for (const auto& win : windows) {
            Config c = night(role, 90);
            c.startMinUtc = win[0];
            c.wakeMinUtc = win[1];
            const Window w = effectiveWindow(c);
            const std::uint32_t lengthS = windowLengthS(w);
            for (std::int64_t t = at(0, 0, 0); t < at(1, 0, 0); t += 37) {
                const std::uint32_t h = static_cast<std::uint32_t>(t * 2654435761u);
                const Decision d = decideAt(c, t, h);
                if (d.action != Action::Sleep) continue;
                CHECK(w.valid);
                CHECK(secondsUntilWindowEnd(w, t) > 0);
                CHECK(d.totalS <= lengthS);
                CHECK(d.sleepS <= d.totalS);
                CHECK(d.sleepS <= c.maxChunkS);
                CHECK(d.sleepS >= kMinSleepS);
                CHECK_EQ(d.wakeAtEpoch, t + d.totalS);
                const std::uint32_t wakeSod = secondOfDay(d.wakeAtEpoch);
                const std::uint32_t early = (w.wakeMin * 60u + kSecondsPerDay - wakeSod) % kSecondsPerDay;
                CHECK(early <= 90);
            }
        }
    }
}

void testChunking() {
    Config c = night(Role::Leaf);
    Decision d = decideAt(c, at(0, 22, 0, 0));
    CHECK_EQ(d.sleepS, 3600);
    c.maxChunkS = 600;
    d = decideAt(c, at(0, 22, 0, 0));
    CHECK_EQ(d.sleepS, 600);
    CHECK_EQ(d.totalS, 8 * 3600);
    c.maxChunkS = 86400;
    d = decideAt(c, at(0, 22, 0, 0));
    CHECK_EQ(d.sleepS, 8 * 3600);
    d = decideAt(c, at(1, 5, 58, 20));
    CHECK_EQ(d.sleepS, 100);
}

void testClockValidity() {
    Config c = night(Role::Leaf);
    Decision d = decideAt(c, 0);
    CHECK(d.action == Action::StayAwake);
    CHECK(d.reason == Reason::NoTime);
    d = decideAt(c, kMinSaneEpoch - 1);
    CHECK(d.reason == Reason::NoTime);
    // 1970-01-01T23:00Z is "in the window" by minute of day, but it is no time.
    d = decideAt(c, 23 * 3600);
    CHECK(d.reason == Reason::NoTime);

    AwakeInput in;
    in.utcEpoch = at(0, 23, 0, 0);
    in.timeValid = false;
    CHECK(decide(c, in).reason == Reason::NoTime);

    // A clock older than the firmware is wrong, even inside the window.
    c.buildEpoch = at(1, 12, 0, 0);
    d = decideAt(c, at(0, 23, 0, 0));
    CHECK(d.action == Action::StayAwake);
    CHECK(d.reason == Reason::OldEpoch);
    d = decideAt(c, at(1, 23, 0, 0));
    CHECK(d.action == Action::Sleep);

    // Grace after a cold boot delays the schedule only.
    c = night(Role::Leaf);
    in = AwakeInput{};
    in.utcEpoch = at(0, 23, 0, 0);
    in.graceActive = true;
    CHECK(decide(c, in).reason == Reason::Grace);
    Config s = survival(true);
    in.batteryMv = 3300;
    CHECK(decide(s, in).action == Action::SurvivalHold);

    // Schedule not built in.
    c.scheduleEnabled = false;
    CHECK(decideAt(c, at(0, 23, 0, 0)).reason == Reason::Disabled);
    c = night(Role::Leaf);
    c.wakeMinUtc = c.startMinUtc;
    CHECK(decideAt(c, at(0, 23, 0, 0)).reason == Reason::NoWindow);
}

void testSurvivalHysteresis() {
    const Config c = survival();
    Decision d = decideAt(c, at(0, 12, 0, 0), 0, 3401);
    CHECK(d.action == Action::StayAwake);
    d = decideAt(c, at(0, 12, 0, 0), 0, 3400);
    CHECK(d.action == Action::SurvivalHold);
    CHECK(d.reason == Reason::BatteryLow);
    CHECK_EQ(d.totalS, 1800);
    CHECK_EQ(d.sleepS, 1800);
    CHECK_EQ(d.wakeAtEpoch, at(0, 12, 30, 0));

    // Between the thresholds the answer depends on the hold.
    CHECK(decideAt(c, at(0, 12, 0, 0), 0, 3550, false).action == Action::StayAwake);
    d = decideAt(c, at(0, 12, 0, 0), 0, 3550, true);
    CHECK(d.action == Action::SurvivalHold);
    CHECK(d.reason == Reason::BatteryHold);
    CHECK(decideAt(c, at(0, 12, 0, 0), 0, 3699, true).action == Action::SurvivalHold);
    CHECK(decideAt(c, at(0, 12, 0, 0), 0, 3700, true).action == Action::StayAwake);

    // Missing or implausible readings never cause sleep, even in a hold.
    const std::int32_t bad[] = {0, -1, 2999, 4501, 65535};
    for (const std::int32_t mv : bad) {
        CHECK(decideAt(c, at(0, 12, 0, 0), 0, mv, true).action == Action::StayAwake);
        CHECK(decideAt(c, at(0, 12, 0, 0), 0, mv, false).action == Action::StayAwake);
    }

    // No clock needed; wakeAt is then unknown.
    d = decideAt(c, 0, 0, 3300);
    CHECK(d.action == Action::SurvivalHold);
    CHECK_EQ(d.wakeAtEpoch, 0);

    // Survival off: a low cell does nothing.
    Config off = c;
    off.survivalEnabled = false;
    CHECK(decideAt(off, at(0, 12, 0, 0), 0, 3100).action == Action::StayAwake);

    // The hold wins over the schedule.
    const Config both = survival(true);
    d = decideAt(both, at(0, 23, 0, 0), 0, 3300);
    CHECK(d.action == Action::SurvivalHold);
    CHECK(decideAt(both, at(0, 23, 0, 0), 0, 3900).action == Action::Sleep);

    // A recheck longer than a chunk is chunked.
    Config longRecheck = c;
    longRecheck.survivalRecheckMin = 120;
    d = decideAt(longRecheck, at(0, 12, 0, 0), 0, 3300);
    CHECK_EQ(d.totalS, 7200);
    CHECK_EQ(d.sleepS, 3600);

    LowBatteryDebounce debounce;
    CHECK(!debounce.update(true));
    CHECK(debounce.update(true));
    CHECK(!debounce.update(false));
    CHECK(!debounce.update(true));
    CHECK(debounce.update(true));
    CHECK(debounce.update(true));
}

void testBootColdAndReset() {
    const Config c = night(Role::Leaf);
    Retained r;
    BootDecision b = bootAt(c, r, at(0, 23, 0, 0));
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ColdBoot);
    CHECK_EQ(b.finished.magic, 0);

    // Leftover RTC garbage with a wrong magic is ignored and cleared.
    r.magic = 0x12345678;
    r.mode = 1;
    r.wakeAtEpoch = at(1, 6, 0, 0);
    b = bootAt(c, r, at(0, 23, 0, 0));
    CHECK(b.cause == WakeCause::ColdBoot);
    CHECK_EQ(r.magic, 0);
    CHECK_EQ(r.wakeAtEpoch, 0);

    // Right magic, unknown mode.
    r.magic = kRetainedMagic;
    r.mode = 7;
    b = bootAt(c, r, at(0, 23, 0, 0));
    CHECK(b.cause == WakeCause::ColdBoot);
    CHECK_EQ(r.magic, 0);

    // Armed, but a brownout, panic or button woke the duck: boot normally.
    r = armedAt(c, at(0, 22, 0, 0));
    CHECK_EQ(r.magic, kRetainedMagic);
    CHECK_EQ(r.mode, static_cast<int>(Mode::Scheduled));
    CHECK_EQ(r.chunks, 1);
    b = bootAt(c, r, at(0, 22, 30, 0), 0, false);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::Reset);
    CHECK_EQ(b.finished.chunks, 1);
    CHECK_EQ(r.magic, 0);
}

void testBootEarlyWakeResleep() {
    const Config c = night(Role::Leaf);
    Retained r = armedAt(c, at(0, 22, 0, 0));
    CHECK_EQ(r.wakeAtEpoch, at(1, 6, 0, 0));
    CHECK_EQ(r.sleptAtEpoch, at(0, 22, 0, 0));

    // The soak's worst case: a timer wake 1112 s before the planned wake.
    BootDecision b = bootAt(c, r, at(1, 6, 0, 0) - 1112);
    CHECK(b.resleep);
    CHECK_EQ(b.sleepS, 1112);
    CHECK_EQ(r.chunks, 2);
    CHECK_EQ(r.mode, static_cast<int>(Mode::Scheduled));

    // Less than a minute early: close enough, wake up.
    b = bootAt(c, r, at(1, 6, 0, 0) - 59);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ScheduleEnd);
    CHECK_EQ(b.finished.chunks, 2);
    CHECK_EQ(b.finished.sleptAtEpoch, at(0, 22, 0, 0));
    CHECK_EQ(r.magic, 0);

    // Late is fine too.
    r = armedAt(c, at(0, 22, 0, 0));
    b = bootAt(c, r, at(1, 6, 5, 0));
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ScheduleEnd);
}

void testBootChunkWalk() {
    // On time: 22:00 to 06:00 in 3600 s chunks is 8 deep sleeps.
    const Config c = night(Role::Leaf);
    Retained r = armedAt(c, at(0, 22, 0, 0));
    std::int64_t now = at(0, 22, 0, 0);
    std::uint32_t sleepS = decideAt(c, now).sleepS;
    int boots = 0;
    for (;;) {
        now += sleepS;
        const BootDecision b = bootAt(c, r, now);
        ++boots;
        if (!b.resleep) {
            CHECK(b.cause == WakeCause::ScheduleEnd);
            CHECK_EQ(b.finished.chunks, 8);
            break;
        }
        CHECK(b.sleepS <= 3600);
        sleepS = b.sleepS;
        if (boots > 50) break;
    }
    CHECK_EQ(boots, 8);
    CHECK_EQ(now, at(1, 6, 0, 0));

    // RC drift: every chunk wakes 3.6 % early by the clock. The duck still
    // wakes for real less than a minute before the planned time, and no
    // single sleep is longer than a chunk.
    r = armedAt(c, at(0, 22, 0, 0));
    now = at(0, 22, 0, 0);
    sleepS = decideAt(c, now).sleepS;
    boots = 0;
    for (;;) {
        now += sleepS - (sleepS * 36) / 1000;
        const BootDecision b = bootAt(c, r, now);
        ++boots;
        if (!b.resleep) {
            CHECK(b.cause == WakeCause::ScheduleEnd);
            break;
        }
        CHECK(b.sleepS <= 3600);
        CHECK(b.sleepS >= kMinSleepS);
        sleepS = b.sleepS;
        if (boots > 50) break;
    }
    CHECK(boots > 8);
    CHECK(boots < 20);
    CHECK(now <= at(1, 6, 0, 0));
    CHECK(now > at(1, 6, 0, 0) - static_cast<std::int64_t>(kMinSleepS));
}

void testBootClockProblems() {
    const Config c = night(Role::Leaf);
    Retained r = armedAt(c, at(0, 22, 0, 0));
    BootDecision b = bootAt(c, r, 0);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ClockLost);
    CHECK_EQ(r.magic, 0);

    // The clock went back: the target is further away than the window is long.
    r = armedAt(c, at(0, 22, 0, 0));
    b = bootAt(c, r, at(0, 21, 0, 0));
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ClockLost);

    // Exactly the window length away is still allowed (armed at the window start).
    r = armedAt(c, at(0, 22, 0, 0));
    b = bootAt(c, r, at(0, 22, 0, 0));
    CHECK(b.resleep);
    CHECK_EQ(b.sleepS, 3600);

    // A clock older than the build at wake time.
    Config built = c;
    built.buildEpoch = at(0, 20, 0, 0);
    r = armedAt(built, at(0, 22, 0, 0));
    b = bootAt(built, r, at(0, 19, 0, 0));
    CHECK(b.cause == WakeCause::ClockLost);
}

void testBootSurvival() {
    const Config c = survival();
    Retained r;
    Decision hold = decideAt(c, at(0, 12, 0, 0), 0, 3300);
    armRetained(r, hold, at(0, 12, 0, 0), 77, false);
    CHECK_EQ(r.mode, static_cast<int>(Mode::Survival));
    CHECK_EQ(r.idHash, 77);

    // Still below the wake threshold: back to sleep, silently.
    BootDecision b = bootAt(c, r, at(0, 12, 30, 0), 3600);
    CHECK(b.resleep);
    CHECK_EQ(b.sleepS, 1800);
    CHECK_EQ(r.chunks, 2);
    CHECK_EQ(r.wakeAtEpoch, at(0, 13, 0, 0));

    // Without a clock the hold goes on all the same.
    b = bootAt(c, r, 0, 3600);
    CHECK(b.resleep);
    CHECK_EQ(r.wakeAtEpoch, 0);

    // Recovered, outside any window: wake for real.
    b = bootAt(c, r, at(0, 13, 30, 0), 3750);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::BatteryRecovered);
    CHECK_EQ(b.finished.chunks, 3);
    CHECK_EQ(r.magic, 0);

    // No early battery reading: boot, and keep the hold's wake threshold.
    armRetained(r, hold, at(0, 12, 0, 0), 77, false);
    b = bootAt(c, r, at(0, 12, 30, 0), 0);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::BatteryCheck);
    CHECK(b.holdActive);

    // Recovered inside the schedule window: straight into the scheduled sleep,
    // keeping the start time and chunk count of the whole night.
    const Config both = survival(true);
    r = Retained{};
    hold = decideAt(both, at(0, 21, 0, 0), 77, 3300);
    armRetained(r, hold, at(0, 21, 0, 0), 77, false);
    b = bootAt(both, r, at(0, 23, 0, 0), 3800);
    CHECK(b.resleep);
    CHECK_EQ(b.sleepS, 3600);
    CHECK_EQ(r.mode, static_cast<int>(Mode::Scheduled));
    CHECK_EQ(r.sleptAtEpoch, at(0, 21, 0, 0));
    CHECK_EQ(r.chunks, 2);
    CHECK_EQ(r.wakeAtEpoch, at(1, 6, 0, 0));

    // The scheduled sleep ends with a flat cell: into the survival hold without booting.
    r = armedAt(both, at(0, 22, 0, 0));
    b = bootAt(both, r, at(1, 6, 0, 0), 3350);
    CHECK(b.resleep);
    CHECK_EQ(b.sleepS, 1800);
    CHECK_EQ(r.mode, static_cast<int>(Mode::Survival));

    // ...and with no early reading it wakes normally.
    r = armedAt(both, at(0, 22, 0, 0));
    b = bootAt(both, r, at(1, 6, 0, 0), 0);
    CHECK(!b.resleep);
    CHECK(b.cause == WakeCause::ScheduleEnd);
    CHECK(!b.holdActive);
}

void testArmRetained() {
    const Config c = night(Role::Leaf);
    Retained r;
    const Decision d = decideAt(c, at(0, 22, 0, 0), 5);
    armRetained(r, d, at(0, 22, 0, 0), 5, false);
    CHECK_EQ(r.magic, kRetainedMagic);
    CHECK_EQ(r.chunks, 1);
    CHECK_EQ(r.plannedS, d.totalS);
    CHECK_EQ(r.sleptAtEpoch, at(0, 22, 0, 0));
    // Arming without a clock records no start time.
    Retained noClock;
    const Decision hold = decideAt(survival(), 0, 0, 3300);
    armRetained(noClock, hold, 0, 5, false);
    CHECK_EQ(noClock.sleptAtEpoch, 0);
    CHECK_EQ(noClock.wakeAtEpoch, 0);
    // The chunk counter saturates instead of wrapping.
    r.chunks = 0xFFFF;
    armRetained(r, d, at(0, 22, 0, 0), 5, true);
    CHECK_EQ(r.chunks, 0xFFFF);
}

}  // namespace

int main() {
    testHelpers();
    testWindows();
    testScheduleCrossingMidnight();
    testRelayOffsets();
    testJitterBounds();
    testNeverLongerThanWindow();
    testChunking();
    testClockValidity();
    testSurvivalHysteresis();
    testBootColdAndReset();
    testBootEarlyWakeResleep();
    testBootChunkWalk();
    testBootClockProblems();
    testBootSurvival();
    testArmRetained();
    return checkSummary("SleepPolicy");
}
