import { TIMEZONES } from "../data/timezones";

// The device stores at most this many bytes (TIMEZONE_MAX_LEN - 1).
export const TIMEZONE_MAX_BYTES = 63;

// Picker entries for rules the IANA table doesn't contain. They stand for
// whatever rule is already stored, so selecting one changes nothing.
export const FIXED_OFFSET_ZONE = "__fixed_offset__";
export const CUSTOM_ZONE = "__custom__";

// Mirrors the firmware's checks (main/utils.c). Neither side parses POSIX
// rules: newlib's tzset() has no error return, so only the shape is checked.
export function validateTimezone(rule) {
  if (typeof rule !== "string" || rule.length === 0) {
    return "Enter a time zone rule";
  }
  if (!/^[\x20-\x7e]+$/.test(rule)) {
    return "Time zone rule must be printable ASCII";
  }
  if (rule.length > TIMEZONE_MAX_BYTES) {
    return `Time zone rule is too long (max ${TIMEZONE_MAX_BYTES} characters)`;
  }
  return "";
}

export function ruleForZone(name) {
  const rule = TIMEZONES[name];
  return typeof rule === "string" ? rule : null;
}

// Zones the frame will not follow exactly, with how far off it gets; the
// picker says so under the zone. Everything else in the table is what tzdata
// itself predicts for the years ahead.
const GREENLAND_NOTE =
  "On the frame, summer time here starts one hour late (Sunday 00:00 rather than Saturday 23:00): its tzset() cannot read the exact rule.";
const PALESTINE_NOTE =
  "Palestine announces its clock changes year by year; this rule is the tz database's prediction and may miss a change.";
export const APPROXIMATE_ZONES = {
  "America/Godthab": GREENLAND_NOTE,
  "America/Nuuk": GREENLAND_NOTE,
  "America/Scoresbysund": GREENLAND_NOTE,
  "Asia/Gaza": PALESTINE_NOTE,
  "Asia/Hebron": PALESTINE_NOTE,
};

// UTC±H[:MM] is what the webapp wrote before it had a zone picker. The POSIX
// sign is inverted from the everyday one: UTC-8 is eight hours ahead of UTC.
const FIXED_OFFSET_RE = /^UTC([+-]?)(\d{1,2})(?::(\d{2}))?$/;

// Everyday-notation label ("UTC+8", "UTC-5:30") for a fixed-offset rule, or
// null when the rule has any other shape.
export function fixedOffsetLabel(rule) {
  const m = FIXED_OFFSET_RE.exec(rule ?? "");
  if (!m) return null;
  const hours = Number(m[2]);
  const minutes = m[3] ? Number(m[3]) : 0;
  if (hours === 0 && minutes === 0) return "UTC+0";
  const sign = m[1] === "-" ? "+" : "-";
  const mm = minutes ? `:${String(minutes).padStart(2, "0")}` : "";
  return `UTC${sign}${hours}${mm}`;
}

// The factory default is UTC0, and "Etc/UCT" sorts ahead of "Etc/UTC".
const PREFERRED_ZONE = { UTC0: "Etc/UTC" };

// The picker entry to show for a stored rule. Many zones share one rule
// (CST-8 is Shanghai, Taipei, Macau, ...), so prefer the browser's own zone
// when it fits, else the first in the table.
export function zoneForRule(rule, browserZone) {
  if (browserZone && ruleForZone(browserZone) === rule) return browserZone;
  if (PREFERRED_ZONE[rule]) return PREFERRED_ZONE[rule];
  for (const [name, zoneRule] of Object.entries(TIMEZONES)) {
    if (zoneRule === rule) return name;
  }
  return fixedOffsetLabel(rule) ? FIXED_OFFSET_ZONE : CUSTOM_ZONE;
}

export function browserTimeZone() {
  try {
    return Intl.DateTimeFormat().resolvedOptions().timeZone || "";
  } catch {
    return "";
  }
}
