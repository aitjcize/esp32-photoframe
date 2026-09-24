import { describe, it, expect } from "vitest";
import { TIMEZONES } from "../data/timezones";
import {
  APPROXIMATE_ZONES,
  CUSTOM_ZONE,
  FIXED_OFFSET_ZONE,
  TIMEZONE_MAX_BYTES,
  fixedOffsetLabel,
  ruleForZone,
  validateTimezone,
  zoneForRule,
} from "./timezone";

describe("validateTimezone", () => {
  it("accepts fixed offsets and full POSIX rules", () => {
    expect(validateTimezone("UTC0")).toBe("");
    expect(validateTimezone("UTC-8")).toBe("");
    expect(validateTimezone("UTC+5:30")).toBe("");
    expect(validateTimezone("EST5EDT,M3.2.0,M11.1.0")).toBe("");
    expect(validateTimezone("<+0330>-3:30")).toBe("");
  });

  it("rejects what the firmware rejects", () => {
    expect(validateTimezone("")).not.toBe("");
    expect(validateTimezone(undefined)).not.toBe("");
    expect(validateTimezone("CET-1\n")).not.toBe("");
    expect(validateTimezone("CET–1")).not.toBe("");
    expect(validateTimezone("A".repeat(TIMEZONE_MAX_BYTES))).toBe("");
    expect(validateTimezone("A".repeat(TIMEZONE_MAX_BYTES + 1))).not.toBe("");
  });

  it("accepts every rule in the picker table", () => {
    for (const rule of Object.values(TIMEZONES)) {
      expect(validateTimezone(rule)).toBe("");
    }
  });
});

describe("APPROXIMATE_ZONES", () => {
  it("names only zones the picker offers", () => {
    for (const name of Object.keys(APPROXIMATE_ZONES)) {
      expect(ruleForZone(name)).not.toBeNull();
    }
  });
});

describe("ruleForZone", () => {
  it("looks up only real zones", () => {
    expect(ruleForZone("Europe/Berlin")).toBe("CET-1CEST,M3.5.0,M10.5.0/3");
    expect(ruleForZone("Nowhere/Town")).toBeNull();
    expect(ruleForZone("constructor")).toBeNull();
  });
});

describe("fixedOffsetLabel", () => {
  it("inverts the POSIX sign", () => {
    expect(fixedOffsetLabel("UTC-8")).toBe("UTC+8");
    expect(fixedOffsetLabel("UTC+5")).toBe("UTC-5");
    expect(fixedOffsetLabel("UTC5")).toBe("UTC-5");
    expect(fixedOffsetLabel("UTC+5:30")).toBe("UTC-5:30");
    expect(fixedOffsetLabel("UTC0")).toBe("UTC+0");
  });

  it("is null for anything but UTC±H[:MM]", () => {
    expect(fixedOffsetLabel("EST5EDT,M3.2.0,M11.1.0")).toBeNull();
    expect(fixedOffsetLabel("UTC-8junk")).toBeNull();
    expect(fixedOffsetLabel("")).toBeNull();
    expect(fixedOffsetLabel(undefined)).toBeNull();
  });
});

describe("zoneForRule", () => {
  const rule = "CST-8"; // shared by Shanghai, Taipei, Macau, ...

  it("prefers the browser's zone when it has the same rule", () => {
    expect(zoneForRule(rule, "Asia/Taipei")).toBe("Asia/Taipei");
  });

  it("falls back to the first zone with the rule", () => {
    const first = Object.keys(TIMEZONES).find((name) => TIMEZONES[name] === rule);
    expect(zoneForRule(rule, "Europe/Berlin")).toBe(first);
    expect(zoneForRule(rule, "")).toBe(first);
  });

  it("shows the factory default as Etc/UTC", () => {
    expect(zoneForRule("UTC0", "Asia/Taipei")).toBe("Etc/UTC");
    expect(zoneForRule("UTC0", "Etc/Zulu")).toBe("Etc/Zulu");
  });

  it("classifies rules the table doesn't have", () => {
    expect(zoneForRule("UTC-8", "Asia/Taipei")).toBe(FIXED_OFFSET_ZONE);
    expect(zoneForRule("XYZ3ABC,M1.1.0,M2.1.0", "Asia/Taipei")).toBe(CUSTOM_ZONE);
    expect(zoneForRule("", "Asia/Taipei")).toBe(CUSTOM_ZONE);
  });
});
