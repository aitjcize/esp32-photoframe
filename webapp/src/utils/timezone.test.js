import { describe, expect, it } from "vitest";
import {
  formatDeviceWallClock,
  formatUtcOffset,
  loadTimezone,
  parseDeviceWallClock,
  parseUtcOffset,
  timezoneForSave,
  timezoneValueFromInput,
} from "./timezone.js";

const AMSTERDAM = "CET-1CEST,M3.5.0/2,M10.5.0/3";

describe("loadTimezone", () => {
  it("keeps a CET/CEST POSIX string and does not report offset 0", () => {
    const loaded = loadTimezone(AMSTERDAM);
    expect(loaded.timezone).toBe(AMSTERDAM);
    expect(loaded.timezoneOffset).toBeNull();
    expect(loaded.timezoneOffset).not.toBe(0);
  });

  it("parses simple POSIX UTC offsets with inverted sign", () => {
    expect(loadTimezone("UTC-1")).toEqual({ timezone: "UTC-1", timezoneOffset: 1 });
    expect(loadTimezone("UTC8")).toEqual({ timezone: "UTC8", timezoneOffset: -8 });
    expect(loadTimezone("UTC+8")).toEqual({ timezone: "UTC+8", timezoneOffset: -8 });
    expect(loadTimezone("UTC0")).toEqual({ timezone: "UTC0", timezoneOffset: 0 });
    expect(loadTimezone("UTC-5:30")).toEqual({ timezone: "UTC-5:30", timezoneOffset: 5.5 });
  });

  it("falls back to UTC0 only when the device sent nothing", () => {
    expect(loadTimezone("")).toEqual({ timezone: "UTC0", timezoneOffset: 0 });
    expect(loadTimezone(undefined)).toEqual({ timezone: "UTC0", timezoneOffset: 0 });
  });
});

describe("timezoneForSave", () => {
  it("preserves a DST POSIX string even if the leftover offset is 0", () => {
    expect(timezoneForSave(AMSTERDAM, 0)).toBe(AMSTERDAM);
    expect(timezoneForSave(AMSTERDAM, null)).toBe(AMSTERDAM);
  });

  it("saves a simple UTC offset string as-is", () => {
    expect(timezoneForSave("UTC-2", 2)).toBe("UTC-2");
  });

  it("rebuilds from the numeric offset only when the string is empty", () => {
    expect(timezoneForSave("", 1)).toBe("UTC-1");
    expect(timezoneForSave("   ", -8)).toBe("UTC+8");
  });
});

describe("formatUtcOffset / parseUtcOffset", () => {
  it("round-trips common offsets", () => {
    expect(parseUtcOffset(formatUtcOffset(1))).toBe(1);
    expect(parseUtcOffset(formatUtcOffset(-8))).toBe(-8);
    expect(parseUtcOffset(formatUtcOffset(5.5))).toBe(5.5);
    expect(formatUtcOffset(0)).toBe("UTC0");
  });

  it("returns null for named POSIX TZ so the UI cannot show a fake 0", () => {
    expect(parseUtcOffset(AMSTERDAM)).toBeNull();
    expect(parseUtcOffset("EST5EDT,M3.2.0/2,M11.1.0/2")).toBeNull();
  });
});

describe("timezoneValueFromInput", () => {
  it("unwraps a preset object and trims typed strings", () => {
    expect(timezoneValueFromInput({ title: "Amsterdam (CET/CEST)", value: AMSTERDAM })).toBe(
      AMSTERDAM
    );
    expect(timezoneValueFromInput(`  ${AMSTERDAM}  `)).toBe(AMSTERDAM);
  });
});

describe("device wall clock", () => {
  it("parses and formats the /api/time wall-clock string", () => {
    const d = parseDeviceWallClock("2026-07-12 14:05:09");
    expect(d).not.toBeNull();
    expect(formatDeviceWallClock(d)).toBe("2026-07-12 14:05:09");
  });
});
