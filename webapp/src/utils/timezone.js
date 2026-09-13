// POSIX TZ helpers for Settings. The device stores the raw string (UTC±H[:MM]
// or a full DST rule such as CET-1CEST,...). The old UI only understood UTC±
// and silently showed 0 for anything else, which would smash the value to UTC0
// on save.

export const TIMEZONE_PRESETS = [
  { title: "UTC", value: "UTC0" },
  { title: "UTC+1 (fixed, no DST)", value: "UTC-1" },
  { title: "UTC+2 (fixed, no DST)", value: "UTC-2" },
  { title: "Amsterdam (CET/CEST)", value: "CET-1CEST,M3.5.0/2,M10.5.0/3" },
  { title: "UTC−5 (Eastern, no DST)", value: "UTC5" },
  { title: "UTC−8 (Pacific, no DST)", value: "UTC8" },
  { title: "UTC+8 (China)", value: "UTC-8" },
  { title: "UTC+5:30 (India)", value: "UTC-5:30" },
];

const UTC_OFFSET_RE = /^UTC([+-]?)(\d+)(?::(\d+))?$/i;

/**
 * Parse a simple POSIX `UTC±H[:MM]` string into an ISO-style hour offset.
 * POSIX sign is inverted (`UTC-1` means UTC+1). Returns null for named /
 * DST-aware strings so the UI does not pretend they are UTC+0.
 */
export function parseUtcOffset(timezone) {
  if (typeof timezone !== "string") return null;
  const match = timezone.trim().match(UTC_OFFSET_RE);
  if (!match) return null;
  const sign = match[1] === "-" ? 1 : -1;
  const hours = parseInt(match[2], 10) || 0;
  const minutes = parseInt(match[3], 10) || 0;
  const offset = sign * (hours + minutes / 60);
  return offset === 0 ? 0 : offset;
}

/** Convert an ISO-style hour offset back to a POSIX `UTC±H[:MM]` string. */
export function formatUtcOffset(offsetValue) {
  const n = Number(offsetValue);
  if (!Number.isFinite(n) || n === 0) return "UTC0";
  const absOffset = Math.abs(n);
  const hours = Math.floor(absOffset);
  const minutes = Math.round((absOffset - hours) * 60);
  const sign = n > 0 ? "-" : "+";
  if (minutes === 0) return `UTC${sign}${hours}`;
  return `UTC${sign}${hours}:${String(minutes).padStart(2, "0")}`;
}

export function isSimpleUtcTimezone(timezone) {
  return parseUtcOffset(timezone) !== null;
}

export function loadTimezone(timezone) {
  const raw = typeof timezone === "string" && timezone.trim() ? timezone.trim() : "UTC0";
  return {
    timezone: raw,
    timezoneOffset: parseUtcOffset(raw),
  };
}

/**
 * POSIX string to persist. Prefer the configured string; only fall back to a
 * numeric offset when the string is missing. Never invent UTC0 for a DST TZ.
 */
export function timezoneForSave(timezone, timezoneOffset) {
  if (typeof timezone === "string") {
    const trimmed = timezone.trim();
    if (trimmed) return trimmed;
  }
  if (timezoneOffset != null && timezoneOffset !== "") {
    return formatUtcOffset(timezoneOffset);
  }
  return "UTC0";
}

/** Parse `/api/time` wall-clock `YYYY-MM-DD HH:MM:SS` as a naive local Date. */
export function parseDeviceWallClock(timeStr) {
  if (!timeStr) return null;
  const match = String(timeStr).match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/);
  if (!match) return null;
  return new Date(
    Number(match[1]),
    Number(match[2]) - 1,
    Number(match[3]),
    Number(match[4]),
    Number(match[5]),
    Number(match[6])
  );
}

export function formatDeviceWallClock(date) {
  const pad = (n) => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

/** Combobox items may be the preset object or a typed string. */
export function timezoneValueFromInput(value) {
  if (value && typeof value === "object" && typeof value.value === "string") {
    return value.value;
  }
  if (typeof value === "string") {
    return value.trim();
  }
  return "";
}
