import { describe, it, expect } from "vitest";
import { parseCron, isValidCron, nextRuns, compileCard, cardFromCron, newCard } from "./cron.js";

const dow = (e) => [...parseCron(e).dow].sort((a, b) => a - b);
const hour = (e) => [...parseCron(e).hour].sort((a, b) => a - b);
const minute = (e) => [...parseCron(e).minute].sort((a, b) => a - b);

// Seconds until the first run, computed from a local wall-clock time. The delta
// between two instants is timezone-independent, so these assertions don't
// depend on the runner's TZ.
function secsUntil(exprs, [y, mo, d, h, mi, s]) {
  const from = new Date(y, mo - 1, d, h, mi, s);
  const runs = nextRuns(exprs, from, 1);
  return runs.length ? Math.round((runs[0].getTime() - from.getTime()) / 1000) : null;
}

describe("parseCron — valid", () => {
  it.each([
    "0 */12 *",
    "*/30 8-22 1-5",
    "0,15,30,45 * *",
    "0 0 *",
    "0 9,12,18 0,6",
    "0 0 7", // 7 = Sunday alias
    "  0   *  *  ", // extra whitespace
    "5/10 * *", // step from bare value
    "0 0 5-7", // Vixie: Fri-Sun
    "0 0 0-7", // Vixie: every day
  ])("accepts %s", (e) => expect(isValidCron(e)).toBe(true));
});

describe("parseCron — invalid", () => {
  it.each([
    "", // empty
    "* *", // too few fields
    "* * * *", // too many fields
    "60 * *", // minute out of range
    "* 24 *", // hour out of range
    "* * 8", // dow out of range
    "5-2 * *", // inverted range
    "a * *", // non-numeric
    "*/0 * *", // zero step
    "*-5 * *", // range after '*'
    "0 12\n*", // newline is not a field separator
    "0 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 *", // field > 31 chars
  ])("rejects %j", (e) => expect(isValidCron(e)).toBe(false));
});

describe("day-of-week Vixie semantics", () => {
  it("7 aliases Sunday", () => expect(dow("0 0 7")).toEqual([0]));
  it("5-7 is Fri-Sun", () => expect(dow("0 0 5-7")).toEqual([0, 5, 6]));
  it("0-7 is every day", () => expect(dow("0 0 0-7")).toEqual([0, 1, 2, 3, 4, 5, 6]));
});

describe("field expansion", () => {
  it("bare value with step means 'from a through hi'", () =>
    expect(minute("5/10 * *")).toEqual([5, 15, 25, 35, 45, 55]));
  it("single-hour range with step", () => expect(hour("0 8-8/2 *")).toEqual([8]));
  it("windowed hours with step", () => expect(hour("0 9-19/2 *")).toEqual([9, 11, 13, 15, 17, 19]));
});

describe("nextRuns", () => {
  it("hourly -> top of next hour", () =>
    expect(secsUntil(["0 * *"], [2026, 6, 27, 10, 0, 0])).toBe(3600));
  it("every 12 hours", () => expect(secsUntil(["0 */12 *"], [2026, 6, 27, 10, 0, 0])).toBe(7200));
  it("every 30 minutes", () => expect(secsUntil(["*/30 * *"], [2026, 6, 27, 10, 0, 0])).toBe(1800));
  it("earliest of multiple rules wins", () =>
    expect(secsUntil(["0 12 *", "30 10 *"], [2026, 6, 27, 10, 0, 0])).toBe(1800));
  it("honors an imminent match (no minimum-delay guard)", () =>
    expect(secsUntil(["0 * *"], [2026, 6, 27, 10, 59, 30])).toBe(30));
  it("rounds up a sub-minute start", () =>
    expect(secsUntil(["0 * *"], [2026, 6, 27, 10, 0, 30])).toBe(3570));
  it("returns nothing for no rules", () =>
    expect(nextRuns([], new Date(2026, 5, 27, 10, 0, 0), 4)).toEqual([]));
});

describe("compileCard", () => {
  const mk = (o) => Object.assign(newCard(), o);
  it("hours interval, all day", () =>
    expect(compileCard(mk({ mode: "interval", unit: "hours", every: 2 }))).toEqual(["0 */2 *"]));
  it("windowed hours emit explicit a-b/n", () =>
    expect(
      compileCard(mk({ mode: "interval", unit: "hours", every: 2, fromHour: 8, toHour: 20 }))
    ).toEqual(["0 8-20/2 *"]));
  it("single-hour window with step is not open-ended", () =>
    expect(
      compileCard(mk({ mode: "interval", unit: "hours", every: 2, fromHour: 8, toHour: 8 }))
    ).toEqual(["0 8-8/2 *"]));
  it("minutes interval", () =>
    expect(compileCard(mk({ mode: "interval", unit: "minutes", every: 30 }))).toEqual([
      "*/30 * *",
    ]));
  it("specific times group hours by minute", () =>
    expect(compileCard(mk({ mode: "times", times: ["08:30", "20:30"], days: "weekdays" }))).toEqual(
      ["30 8,20 1-5"]
    ));
  it("cleared/half-typed time inputs are skipped", () =>
    expect(compileCard(mk({ mode: "times", times: ["", "12"] }))).toEqual(["0 * *"]));
  it("emptied advanced field compiles to an invalid empty rule", () =>
    expect(compileCard(mk({ raw: "" }))).toEqual([""]));
});

describe("cardFromCron round-trip", () => {
  it.each([
    ["0 * *", "0 * *"],
    ["* 8-20 *", "* 8-20 *"],
    ["0 8-20 *", "0 8-20/1 *"],
    ["0 */5 *", "0 */5 *"],
    ["*/30 * *", "*/30 * *"],
    ["30 9,18 1-5", "30 9,18 1-5"],
  ])("%s stays a friendly card", (expr, recompiled) => {
    const card = cardFromCron(expr);
    expect(card.raw).toBeNull();
    expect(compileCard(card)).toEqual([recompiled]);
  });
  it("an unmappable expression falls back to a raw card", () => {
    const card = cardFromCron("15 8-20 *");
    expect(card.raw).toBe("15 8-20 *");
  });
});
