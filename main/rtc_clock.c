#include "rtc_clock.h"

#include <time.h>

time_t rtc_clock_from_legacy_local(time_t stored_as_utc)
{
    // Undo the driver's UTC reading to get the raw calendar fields back, then
    // read them the way the old firmware wrote them: as local time, letting
    // mktime() work out from the date which DST offset was in force.
    struct tm fields;
    gmtime_r(&stored_as_utc, &fields);
    fields.tm_isdst = -1;
    return mktime(&fields);
}
