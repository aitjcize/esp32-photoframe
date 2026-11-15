#pragma once

#include <time.h>

// What the external RTC holds and how to read it back.
//
// The RTC chips keep calendar fields (year, month, ..., second) with no notion
// of a time zone. Firmware up to v2.20.1 wrote the local wall-clock time into
// them, DST offset included, and read them back as local time, so after a DST
// changeover every cold boot restored a clock one hour off until the next SNTP
// sync (#121). The drivers now store and read UTC, which has no changeovers.
//
// A frame updated from that firmware still carries local fields in its RTC on
// its first boot. NVS_RTC_UTC_KEY, set once the firmware has written UTC,
// tells the two cases apart; rtc_clock_from_legacy_local() reinterprets the
// old contents so that first boot keeps the right time too.
//
// Kept free of ESP-IDF headers so it is host-tested.

// Set to 1 in NVS_NAMESPACE once the RTC has been written as UTC.
#define NVS_RTC_UTC_KEY "rtc_utc"

// The instant an RTC written by older firmware (local wall-clock fields) really
// denotes. stored_as_utc is what the driver read: those local fields taken as
// UTC. The TZ rule must already be applied (tzset()).
time_t rtc_clock_from_legacy_local(time_t stored_as_utc);
