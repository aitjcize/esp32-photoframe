// Simulates the deep-sleep wake / early-wake re-sleep loop against the *real*
// cron engine to check that a scheduled rotation fires exactly once per cron
// boundary under clock error. Models issue #105: frames on boards with an
// external RTC ("photopainter_73", "E1002") stop rotating on the hourly wakes
// after the first, while the RTC-less "EE02" keeps working.
//
// The model mirrors main.c:deep_sleep_wake_main() + power_manager.c:
//   - on wake the system clock is the external RTC value (restored at boot);
//   - power_manager_init() records wake_target_boundary = expected_wakeup_time
//     and force-syncs NTP when |drift| > 30s;
//   - early-wake check #1 (pre-WiFi) re-sleeps if we woke > 5s early;
//   - periodic tasks may pull the clock to true time via NTP;
//   - early-wake check #2 (post-NTP) re-sleeps if still > 5s early;
//   - otherwise it rotates, then enter_sleep() sets the next timer from cron.
//
// The deep-sleep timer runs off the internal RC oscillator, so the real time
// that elapses for a requested sleep is `requested * osc` (osc < 1 => wakes
// early, osc > 1 => wakes late) even on boards whose RTC keeps perfect time.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>

extern "C" {
#include "cron.h"
}

namespace
{

constexpr int kEarlyTolerance = 5;  // EARLY_WAKE_TOLERANCE_SEC in config.h
constexpr int kDriftForceNtp = 30;  // drift threshold in power_manager.c

std::vector<cron_rule_t> compile(const std::vector<const char *> &exprs)
{
    std::vector<cron_rule_t> rules;
    for (const char *e : exprs) {
        cron_rule_t r;
        EXPECT_TRUE(cron_parse(e, &r)) << "failed to parse: " << e;
        rules.push_back(r);
    }
    return rules;
}

// Seconds from the given wall-clock instant to the next cron match, using the
// exact function the firmware calls in get_seconds_until_next_wakeup().
long cron_next(time_t dev_now, const std::vector<cron_rule_t> &rules)
{
    struct tm lt;
    localtime_r(&dev_now, &lt);
    return cron_seconds_until_next(&lt, rules.data(), (int) rules.size());
}

struct SimConfig {
    double osc = 1.0;             // elapsed_true = requested * osc (RC oscillator error)
    long rtc_offset = 0;          // external RTC reads (true + rtc_offset); 0 = perfect RTC
    bool has_ext_rtc = true;      // false models EE02 (no RTC restore at boot)
    bool ntp_every_wake = false;  // sync NTP on every WiFi-connected wake
    bool ntp_writes_rtc = true;   // NTP writeback corrects the external RTC too
    long rotation_dur = 90;       // seconds awake for a rotation before re-sleeping
    bool needs_wifi = true;       // URL mode / HA configured: NTP can run
};

// One rotation record: the device-clock instant it fired.
std::vector<time_t> simulate(SimConfig cfg, const std::vector<const char *> &exprs,
                             time_t start_true, time_t end_true)
{
    auto rules = compile(exprs);
    std::vector<time_t> rotations;

    long true_t = start_true;
    long rtc_offset = cfg.rtc_offset;

    // Fresh boot: device clock = external RTC (or drifted internal clock if none).
    long dev_now = cfg.has_ext_rtc ? (true_t + rtc_offset) : true_t;
    // No prior target on the first cycle.
    long expected = 0;
    bool have_expected = false;

    int guard = 0;
    const int kGuardMax = 500000;

    while (true_t < end_true && guard++ < kGuardMax) {
        // ---- enter_sleep(): arm timer from cron, record expected wake ----
        long requested = cron_next(dev_now, rules);
        expected = dev_now + requested;
        have_expected = true;

        // ---- deep sleep: real time advances by requested * oscillator error ----
        true_t += (long) std::llround((double) requested * cfg.osc);
        if (true_t >= end_true)
            break;

        // ---- wake: restore clock from external RTC (boot) ----
        dev_now = cfg.has_ext_rtc ? (true_t + rtc_offset) : true_t;

        // ---- power_manager_init(): drift check + forced NTP ----
        long boundary = have_expected ? expected : 0;
        long drift = dev_now - expected;
        bool force_ntp = std::llabs(drift) > kDriftForceNtp;

        // ---- early-wake check #1 (pre-WiFi) ----
        long early1 = (boundary > dev_now) ? (boundary - dev_now) : 0;
        if (early1 > kEarlyTolerance) {
            // re-sleep: loop straight back into enter_sleep() with this clock
            continue;
        }

        // ---- WiFi + periodic tasks (NTP) ----
        if (cfg.needs_wifi && (force_ntp || cfg.ntp_every_wake)) {
            dev_now = true_t;  // NTP pulls to real wall-clock
            if (cfg.ntp_writes_rtc) {
                rtc_offset = 0;
            }
        }

        // ---- early-wake check #2 (post-NTP) ----
        long early2 = (boundary > dev_now) ? (boundary - dev_now) : 0;
        if (early2 > kEarlyTolerance) {
            continue;  // re-sleep
        }

        // ---- ha_notify_online() gate would go here; assume rotate=true ----
        // ---- rotate ----
        rotations.push_back((time_t) dev_now);

        // ---- HA window + hold, then loop into enter_sleep ----
        true_t += cfg.rotation_dur;
        dev_now += cfg.rotation_dur;
    }

    EXPECT_LT(guard, kGuardMax) << "wake loop did not terminate (infinite re-sleep)";
    return rotations;
}

// Reduce rotation instants to the set of cron boundaries they serviced. A
// rotation at HH:MM:SS is credited to hour HH (rotations fire at or within a
// few seconds of the top of the hour). Returns per-hour counts.
std::vector<int> hourly_hits(const std::vector<time_t> &rotations, int lo_hour, int hi_hour)
{
    std::vector<int> hits(24, 0);
    for (time_t r : rotations) {
        struct tm lt;
        localtime_r(&r, &lt);
        // Credit to the nearest top-of-hour (a rotation up to 5s early rounds up).
        int hour = lt.tm_hour;
        if (lt.tm_min >= 59 && lt.tm_sec >= 55) {
            hour = (hour + 1) % 24;
        }
        if (hour >= lo_hour && hour <= hi_hour) {
            hits[hour]++;
        }
    }
    return hits;
}

class WakeSim : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "UTC0", 1);
        tzset();
    }
};

// A day of "hourly 07:00-20:00" — the reporter's schedule.
const std::vector<const char *> kHourly7to20 = {"0 7-20 *"};

// 2026-06-27 06:30:00 UTC .. 2026-06-27 21:00:00 UTC
time_t DayStart()
{
    struct tm t = {};
    t.tm_year = 2026 - 1900;
    t.tm_mon = 5;  // June
    t.tm_mday = 27;
    t.tm_hour = 6;
    t.tm_min = 30;
    t.tm_isdst = -1;
    return mktime(&t);
}
time_t DayEnd()
{
    return DayStart() + (21 - 6) * 3600 - 30 * 60;
}

void ExpectOnePerHour(const SimConfig &cfg, const char *label)
{
    auto rot = simulate(cfg, kHourly7to20, DayStart(), DayEnd());
    auto hits = hourly_hits(rot, 7, 20);
    for (int h = 7; h <= 20; h++) {
        EXPECT_EQ(hits[h], 1) << label << ": hour " << h << " rotated " << hits[h]
                              << " times (expected exactly 1)";
    }
}

TEST_F(WakeSim, PerfectClockRotatesEveryHour)
{
    SimConfig cfg;  // osc = 1.0, perfect RTC
    ExpectOnePerHour(cfg, "perfect");
}

TEST_F(WakeSim, FastOscillatorExternalRtc)
{
    // RC oscillator 3% fast: every 1h sleep wakes ~108s early. External RTC
    // perfect, so early-wake check #1 sees the truth and re-sleeps.
    SimConfig cfg;
    cfg.osc = 0.97;
    ExpectOnePerHour(cfg, "osc-fast+extrtc");
}

TEST_F(WakeSim, SlowOscillatorExternalRtc)
{
    SimConfig cfg;
    cfg.osc = 1.03;  // wakes ~108s late
    ExpectOnePerHour(cfg, "osc-slow+extrtc");
}

TEST_F(WakeSim, NoExternalRtcNtpEveryWake)
{
    // EE02: no RTC restore, but NTP corrects on every WiFi wake. Known-good.
    SimConfig cfg;
    cfg.has_ext_rtc = false;
    cfg.ntp_every_wake = true;
    cfg.osc = 0.97;
    ExpectOnePerHour(cfg, "no-rtc+ntp-each");
}

// The suspected #105 trigger: external RTC runs fast, NTP runs after WiFi and
// yanks the clock *backward* past the boundary between check #1 and check #2.
TEST_F(WakeSim, ExternalRtcFastNtpBackwardCorrection)
{
    SimConfig cfg;
    cfg.osc = 1.0;
    cfg.rtc_offset = 45;         // RTC 45s ahead of real time
    cfg.ntp_every_wake = true;   // NTP pulls clock back to truth each wake
    cfg.ntp_writes_rtc = false;  // ...but never fixes the RTC, so it recurs
    ExpectOnePerHour(cfg, "extrtc-fast+ntp-backward");
}

TEST_F(WakeSim, ExternalRtcFastForcedNtpOnly)
{
    // RTC far enough ahead to trip the >30s drift force-NTP path every wake.
    SimConfig cfg;
    cfg.osc = 1.0;
    cfg.rtc_offset = 90;
    cfg.ntp_every_wake = false;  // only the drift-forced sync runs
    cfg.ntp_writes_rtc = false;
    ExpectOnePerHour(cfg, "extrtc-fast+forced-ntp");
}

TEST_F(WakeSim, ExternalRtcSlowNtpForwardCorrection)
{
    SimConfig cfg;
    cfg.osc = 1.0;
    cfg.rtc_offset = -45;  // RTC 45s behind
    cfg.ntp_every_wake = true;
    cfg.ntp_writes_rtc = false;
    ExpectOnePerHour(cfg, "extrtc-slow+ntp-forward");
}

// @yllar: "does it rotate if you set the interval to 10 or 15 min?"
TEST_F(WakeSim, FifteenMinuteIntervalFastOscillator)
{
    SimConfig cfg;
    cfg.osc = 0.97;
    auto rot = simulate(cfg, {"*/15 * *"}, DayStart(), DayStart() + 3 * 3600);
    // 06:45,07:00,...  over 3h from 06:30 -> at least ~11 quarter-hours.
    EXPECT_GE(rot.size(), 10u) << "15-min interval dropped rotations";
}

}  // namespace
