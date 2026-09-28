import { describe, it, expect } from "vitest";
import {
  orientedSize,
  logicalFrame,
  coverRect,
  fitRect,
  normalizeRect,
  denormalizeRect,
  rectToZoomPan,
  clampRect,
  rectLimits,
  rectHasBars,
  upscaleFactor,
  MAX_UPSCALE,
  MAX_MARGIN,
} from "./framing.js";

const close = (a, b) => expect(Math.abs(a - b)).toBeLessThan(1e-6);

describe("orientedSize / logicalFrame", () => {
  it("swaps dimensions for quarter turns only", () => {
    expect(orientedSize(4000, 3000, 90)).toEqual({ width: 3000, height: 4000 });
    expect(orientedSize(4000, 3000, 270)).toEqual({ width: 3000, height: 4000 });
    expect(orientedSize(4000, 3000, 180)).toEqual({ width: 4000, height: 3000 });
    expect(orientedSize(4000, 3000, -90)).toEqual({ width: 3000, height: 4000 });
  });

  it("orients the native panel size", () => {
    expect(logicalFrame(1200, 1600, "portrait")).toEqual({ width: 1200, height: 1600 });
    expect(logicalFrame(1200, 1600, "landscape")).toEqual({ width: 1600, height: 1200 });
    expect(logicalFrame(800, 480, "portrait")).toEqual({ width: 480, height: 800 });
    expect(logicalFrame(800, 480, "landscape")).toEqual({ width: 800, height: 480 });
  });
});

describe("cover / fit rects", () => {
  it("cover keeps the frame aspect and fits inside the image", () => {
    // 4:3 photo on a 3:4 frame: full height, cropped width
    const r = coverRect(4000, 3000, 1200, 1600);
    close(r.w / r.h, 1200 / 1600);
    close(r.h, 3000);
    close(r.w, 2250);
    close(r.x, (4000 - 2250) / 2);
    close(r.y, 0);
  });

  it("fit keeps the frame aspect and contains the image", () => {
    const r = fitRect(4000, 3000, 1200, 1600);
    close(r.w / r.h, 1200 / 1600);
    close(r.w, 4000);
    close(r.h, 4000 / 0.75);
    close(r.x, 0);
    expect(r.y).toBeLessThan(0);
  });

  it("cover and fit coincide when the photo already has the frame aspect", () => {
    const c = coverRect(600, 800, 1200, 1600);
    const f = fitRect(600, 800, 1200, 1600);
    expect(c).toEqual(f);
    expect(c).toEqual({ x: 0, y: 0, w: 600, h: 800 });
  });
});

describe("normalized rects and zoom / pan", () => {
  it("round-trips through normalization at a different resolution", () => {
    const full = { x: 875, y: 0, w: 2250, h: 3000 };
    const norm = normalizeRect(full, 4000, 3000);
    const small = denormalizeRect(norm, 400, 300);
    close(small.x, 87.5);
    close(small.w, 225);
    close(small.h, 300);
  });

  it("maps a cover rect to a zoom that exactly fills the frame", () => {
    const r = coverRect(4000, 3000, 1200, 1600);
    const { zoom, panX, panY } = rectToZoomPan(r, 1200);
    close(r.w * zoom, 1200);
    close(r.h * zoom, 1600);
    close(panX, -r.x * zoom);
    close(panY, 0);
    // The whole image, drawn at this zoom, is wider than the frame and starts left of it.
    expect(panX).toBeLessThan(0);
    close(panX + 4000 * zoom, 1200 - panX);
  });

  it("maps a fit rect to bars on the uncovered side", () => {
    const r = fitRect(4000, 3000, 1200, 1600);
    const { zoom, panY } = rectToZoomPan(r, 1200);
    close(4000 * zoom, 1200); // image spans the full frame width
    expect(panY).toBeGreaterThan(0); // and is pushed down: a bar above it
    close(panY * 2 + 3000 * zoom, 1600); // centered vertically
  });
});

describe("clampRect", () => {
  const limits = rectLimits(4000, 3000, 1200, 1600);
  const aspect = 1200 / 1600;

  it("keeps a small rect inside the image", () => {
    const r = clampRect({ x: -100, y: 2500, w: 1000, h: 1333 }, 4000, 3000, aspect, limits);
    expect(r.x).toBe(0);
    close(r.y, 3000 - r.h);
    close(r.w / r.h, aspect);
  });

  it("keeps the image inside a rect that is larger than the image", () => {
    const fit = fitRect(4000, 3000, 1200, 1600); // taller than the image
    const r = clampRect({ ...fit, y: 500 }, 4000, 3000, aspect, limits);
    expect(r.y).toBe(0); // image top aligned with the rect top at most
    const r2 = clampRect({ ...fit, y: -100000 }, 4000, 3000, aspect, limits);
    close(r2.y, 3000 - fit.h);
  });

  it("bounds the size between the upscale floor and the margin ceiling", () => {
    const tiny = clampRect({ x: 0, y: 0, w: 10, h: 13 }, 4000, 3000, aspect, limits);
    close(tiny.w, 1200 / MAX_UPSCALE);
    const huge = clampRect({ x: 0, y: 0, w: 1e6, h: 1e6 }, 4000, 3000, aspect, limits);
    close(huge.w, fitRect(4000, 3000, 1200, 1600).w * MAX_MARGIN);
  });

  it("never forbids the cover rect of a tiny photo", () => {
    const lim = rectLimits(200, 150, 1200, 1600);
    const cover = coverRect(200, 150, 1200, 1600);
    close(lim.minW, cover.w);
  });
});

describe("bars and upscale", () => {
  it("detects letterbox bars", () => {
    expect(rectHasBars(coverRect(4000, 3000, 1200, 1600), 4000, 3000)).toBe(false);
    expect(rectHasBars(fitRect(4000, 3000, 1200, 1600), 4000, 3000)).toBe(true);
  });

  it("reports the enlargement factor", () => {
    close(upscaleFactor({ w: 600 }, 1200), 2);
    close(upscaleFactor({ w: 300 }, 1200, 0.5), 2); // rect measured on a half-size copy
  });
});
