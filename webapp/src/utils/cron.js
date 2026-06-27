// Cron schedule helpers shared by the rotation-schedule builder.
//
// Mirrors the firmware engine (main/cron.{c,h}): simplified 3-field expressions
// "minute hour day-of-week", with *, a, a-b, */n, a-b/n and comma lists;
// day-of-week 0/7 = Sunday. Kept dependency free so it can be embedded in device
// flash and copied to the server webapp.

const FIELD_RANGES = [
  [0, 59], // minute
  [0, 23], // hour
  [0, 6], // day of week
];

// Parse one field into a Set of integers, or return null if malformed.
function parseField(field, lo, hi, isDow) {
  if (field === "") return null;
  const out = new Set();
  for (const term of field.split(",")) {
    const m = term.match(/^(\*|\d+)(?:-(\d+))?(?:\/(\d+))?$/);
    if (!m) return null;
    let start;
    let end;
    let step = 1;
    if (m[1] === "*") {
      start = lo;
      end = hi;
    } else {
      start = parseInt(m[1], 10);
      end = m[2] !== undefined ? parseInt(m[2], 10) : start;
      // bare value with a step ("5/10") means "from 5 through hi"
      if (m[2] === undefined && m[3] !== undefined) end = hi;
    }
    if (m[3] !== undefined) {
      step = parseInt(m[3], 10);
      if (step <= 0) return null;
    }
    if (isDow) {
      if (start === 7) start = 0;
      if (end === 7) end = 0;
    }
    if (start < lo || end > hi || start > end) return null;
    for (let v = start; v <= end; v += step) out.add(v);
  }
  return out;
}

// Parse a 3-field cron expression ("minute hour day-of-week") into compiled
// field sets, or null if invalid.
export function parseCron(expr) {
  if (typeof expr !== "string") return null;
  const fields = expr.trim().split(/\s+/);
  if (fields.length !== 3) return null;
  const sets = [];
  for (let i = 0; i < 3; i++) {
    const s = parseField(fields[i], FIELD_RANGES[i][0], FIELD_RANGES[i][1], i === 2);
    if (!s) return null;
    sets.push(s);
  }
  return { minute: sets[0], hour: sets[1], dow: sets[2] };
}

export function isValidCron(expr) {
  return parseCron(expr) !== null;
}

// Does the local time `d` (a Date) satisfy compiled rule `r`?
function matches(r, d) {
  if (!r.minute.has(d.getMinutes())) return false;
  if (!r.hour.has(d.getHours())) return false;
  return r.dow.has(d.getDay());
}

function inQuietWindow(d, sleep) {
  if (!sleep || !sleep.enabled) return false;
  const cur = d.getHours() * 60 + d.getMinutes();
  const { start, end } = sleep; // minutes since midnight
  if (start > end) return cur >= start || cur < end;
  return cur >= start && cur < end;
}

const HORIZON_MIN = 8 * 24 * 60;

// Next `count` rotation times from `from` (Date), across all cron rules, with an
// optional quiet-hours mask {enabled,start,end}. Mirrors the firmware scan
// (minute granularity).
export function nextRuns(exprs, from = new Date(), count = 5, sleep = null) {
  const rules = exprs.map(parseCron).filter(Boolean);
  if (!rules.length) return [];
  const t0 = from.getTime();
  // start at the next whole minute
  const startMs = Math.ceil((t0 + 1) / 60000) * 60000;
  const runs = [];
  for (let i = 0; i < HORIZON_MIN && runs.length < count; i++) {
    const d = new Date(startMs + i * 60000);
    if (!rules.some((r) => matches(r, d))) continue;
    if (inQuietWindow(d, sleep)) continue;
    runs.push(d);
  }
  return runs;
}

// ----------------------------------------------------------------------------
// Structured builder model  <->  cron strings
//
// A "card" is one friendly rule editor. days + mode compile to one or more
// cron strings (specific-times with differing minutes split into several).
// ----------------------------------------------------------------------------

export const DAY_LABELS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

export function newCard() {
  return {
    days: "everyday", // "everyday" | "weekdays" | "weekends" | number[]
    mode: "interval", // "interval" | "times"
    every: 12,
    unit: "hours", // "hours" | "minutes"
    fromHour: 0,
    toHour: 23,
    times: ["08:00"],
    raw: null, // advanced override (single expression)
  };
}

function daysToCron(days) {
  if (days === "everyday") return "*";
  if (days === "weekdays") return "1-5";
  if (days === "weekends") return "0,6";
  if (Array.isArray(days)) {
    if (days.length === 0 || days.length === 7) return "*";
    return [...days].sort((a, b) => a - b).join(",");
  }
  return "*";
}

function hourRange(fromHour, toHour) {
  if (fromHour <= 0 && toHour >= 23) return "*";
  return fromHour === toHour ? `${fromHour}` : `${fromHour}-${toHour}`;
}

// Compile a single card to an array of cron strings.
export function compileCard(card) {
  if (card.raw) return [card.raw.trim()];
  const dow = daysToCron(card.days);

  if (card.mode === "interval") {
    if (card.unit === "minutes") {
      const min = card.every > 1 ? `*/${card.every}` : "*";
      return [`${min} ${hourRange(card.fromHour, card.toHour)} ${dow}`];
    }
    // hours
    const hr =
      card.fromHour <= 0 && card.toHour >= 23
        ? card.every > 1
          ? `*/${card.every}`
          : "*"
        : `${hourRange(card.fromHour, card.toHour)}/${card.every}`;
    return [`0 ${hr} ${dow}`];
  }

  // specific times: group hours by minute
  const byMinute = new Map();
  for (const t of card.times) {
    const [h, m] = t.split(":").map(Number);
    if (Number.isNaN(h) || Number.isNaN(m)) continue;
    if (!byMinute.has(m)) byMinute.set(m, []);
    byMinute.get(m).push(h);
  }
  const rules = [];
  for (const [m, hours] of byMinute) {
    const uniq = [...new Set(hours)].sort((a, b) => a - b);
    rules.push(`${m} ${uniq.join(",")} ${dow}`);
  }
  return rules.length ? rules : [`0 * ${dow}`];
}

// Compile all cards to the flat cron-rule array sent to the device.
export function compileCards(cards) {
  return cards.flatMap(compileCard);
}

// ----------------------------------------------------------------------------
// cron string -> structured card (best-effort, for loading saved config)
// ----------------------------------------------------------------------------

function cronDowToDays(field) {
  if (field === "*") return "everyday";
  if (field === "1-5") return "weekdays";
  if (field === "0,6" || field === "6,0") return "weekends";
  const set = parseField(field, 0, 6, true);
  if (!set) return "everyday";
  return [...set].sort((a, b) => a - b);
}

// Convert a single stored cron expression into a builder card. Falls back to
// raw (advanced) mode when the expression doesn't map to a friendly shape.
export function cardFromCron(expr) {
  const card = newCard();
  const fields = expr.trim().split(/\s+/);
  if (fields.length !== 3 || !parseCron(expr)) {
    card.raw = expr.trim();
    return card;
  }
  const [minF, hourF, dowF] = fields;
  card.days = cronDowToDays(dowF);

  const stepMin = minF.match(/^\*\/(\d+)$/);
  const stepHour = hourF.match(/^(?:\*|(\d+)-(\d+))\/(\d+)$/);

  if (stepMin && (hourF === "*" || /^\d+-\d+$/.test(hourF))) {
    card.mode = "interval";
    card.unit = "minutes";
    card.every = parseInt(stepMin[1], 10);
    if (hourF === "*") {
      card.fromHour = 0;
      card.toHour = 23;
    } else {
      const [a, b] = hourF.split("-").map(Number);
      card.fromHour = a;
      card.toHour = b;
    }
    return card;
  }

  if (minF === "0" && stepHour) {
    card.mode = "interval";
    card.unit = "hours";
    card.every = parseInt(stepHour[3], 10);
    card.fromHour = stepHour[1] !== undefined ? parseInt(stepHour[1], 10) : 0;
    card.toHour = stepHour[2] !== undefined ? parseInt(stepHour[2], 10) : 23;
    return card;
  }

  if (/^\d+$/.test(minF) && /^[\d,]+$/.test(hourF)) {
    card.mode = "times";
    const m = minF.padStart(2, "0");
    card.times = hourF
      .split(",")
      .map((h) => `${h.padStart(2, "0")}:${m}`)
      .sort();
    return card;
  }

  card.raw = expr.trim();
  return card;
}

// Each stored cron string becomes its own card (friendly when possible).
export function cardsFromCron(exprs) {
  if (!exprs || !exprs.length) return [newCard()];
  return exprs.map(cardFromCron);
}

// ----------------------------------------------------------------------------
// Human-readable summary (from structured state — no reverse parsing)
// ----------------------------------------------------------------------------

function daysLabel(days) {
  if (days === "everyday") return "every day";
  if (days === "weekdays") return "weekdays";
  if (days === "weekends") return "weekends";
  if (Array.isArray(days)) {
    if (days.length === 0 || days.length === 7) return "every day";
    return days
      .slice()
      .sort((a, b) => a - b)
      .map((d) => DAY_LABELS[d])
      .join(", ");
  }
  return "every day";
}

function hour12(h) {
  const ampm = h < 12 ? "AM" : "PM";
  const hr = h % 12 === 0 ? 12 : h % 12;
  return `${hr} ${ampm}`;
}

function timeLabel(hhmm) {
  const [h, m] = hhmm.split(":").map(Number);
  const ampm = h < 12 ? "AM" : "PM";
  const hr = h % 12 === 0 ? 12 : h % 12;
  return `${hr}:${String(m).padStart(2, "0")} ${ampm}`;
}

export function describeCard(card) {
  if (card.raw) return `Custom: ${card.raw}`;
  const days = daysLabel(card.days);
  if (card.mode === "interval") {
    const unit = card.unit === "hours" ? "hours" : "minutes";
    const every =
      card.every === 1
        ? `every ${card.unit === "hours" ? "hour" : "minute"}`
        : `every ${card.every} ${unit}`;
    const window =
      card.fromHour <= 0 && card.toHour >= 23
        ? ""
        : `, ${hour12(card.fromHour)}–${hour12(card.toHour)}`;
    return `Rotate ${every}${window}, ${days}`;
  }
  const times = card.times.map(timeLabel).join(", ");
  return `Rotate at ${times}, ${days}`;
}
