/**
 * Jest test suite for --device-config: reading a config exported by the
 * frame's web UI instead of fetching the same data from a live device.
 */

import fs from "fs";
import os from "os";
import path from "path";
import { spawnSync } from "child_process";
import { gunzipSync } from "zlib";
import { fileURLToPath } from "url";
import { loadImage } from "canvas";
import fetch from "node-fetch";
import { SPECTRA6, makeGrayscale16 } from "@aitjcize/epaper-image-convert";
import {
  parseDeviceConfig,
  loadDeviceConfig,
  isGrayCalibration,
} from "../device-config.js";
import { buildPalette } from "../utils.js";
import { createImageServer } from "../server.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const CLI = path.join(__dirname, "..", "cli.js");
const FIXTURES = path.join(__dirname, "fixtures");
const NEW_STYLE = path.join(FIXTURES, "device-config.json");
const OLD_STYLE = path.join(FIXTURES, "device-config-old.json");
const GRAYSCALE = path.join(FIXTURES, "device-config-grayscale.json");
const IMAGE = path.join(__dirname, "test-albums", "Default", "landscape.jpg");

const readJson = (file) => JSON.parse(fs.readFileSync(file, "utf8"));

function runCli(args, cwd) {
  return spawnSync(process.execPath, [CLI, ...args], {
    cwd,
    encoding: "utf8",
  });
}

describe("parseDeviceConfig", () => {
  test("new-style export: processing, palette, orientation and system_info", () => {
    const raw = readJson(NEW_STYLE);
    const cfg = parseDeviceConfig(raw);

    expect(cfg.processing).toEqual(raw.processing);
    expect(cfg.palette).toEqual(raw.palette);
    expect(cfg.orientation).toBe("portrait");
    expect(cfg.width).toBe(1600);
    expect(cfg.height).toBe(1200);
    expect(cfg.version).toBe("2.9.0");
    expect(cfg.boardName).toBe("seeedstudio_reterminal_e1004");
    expect(cfg.grayscale).toBe(false);
  });

  test("old-style export (no system_info): size unknown, rest as before", () => {
    const raw = readJson(OLD_STYLE);
    const cfg = parseDeviceConfig(raw);

    expect(cfg.processing).toEqual(raw.processing);
    expect(cfg.palette).toEqual(raw.palette);
    expect(cfg.orientation).toBe("landscape");
    expect(cfg.width).toBeNull();
    expect(cfg.height).toBeNull();
    expect(cfg.version).toBe("");
    expect(cfg.boardName).toBeNull();
    expect(cfg.grayscale).toBe(false);
  });

  test("grayscale export: display_type gc16 marks the panel grayscale", () => {
    const raw = readJson(GRAYSCALE);
    const cfg = parseDeviceConfig(raw);

    expect(cfg.grayscale).toBe(true);
    expect(cfg.width).toBe(1872);
    expect(cfg.height).toBe(1404);
    expect(isGrayCalibration(cfg.palette)).toBe(true);
  });

  test("old-style grayscale export: the gray calibration palette marks it", () => {
    const raw = readJson(GRAYSCALE);
    delete raw.system_info;
    const cfg = parseDeviceConfig(raw);

    expect(cfg.grayscale).toBe(true);
    expect(cfg.width).toBeNull();
  });

  test("blocks are optional; unknown keys are ignored", () => {
    const cfg = parseDeviceConfig({
      palette: readJson(OLD_STYLE).palette,
      something_new: { future: true },
    });
    expect(cfg.processing).toBeNull();
    expect(cfg.orientation).toBeNull();
    expect(cfg.palette).toBeTruthy();
  });

  test("config without display_orientation leaves orientation unset", () => {
    const cfg = parseDeviceConfig({
      config: { device_name: "x" },
      processing: readJson(OLD_STYLE).processing,
    });
    expect(cfg.orientation).toBeNull();
  });

  test.each([
    [null, "expected a JSON object"],
    [[], "expected a JSON object"],
    [{ foo: 1 }, "not a device config export"],
    [{ processing: "x" }, '"processing" must be an object'],
    [{ palette: [] }, '"palette" must be an object'],
    [
      { palette: { black: { r: 0, g: 0, b: 0 } } },
      '"palette.white" must be an object',
    ],
    [
      { palette: { black_y: 0.01, white_y: "0.6" } },
      '"palette.black_y" and "palette.white_y" must be numbers',
    ],
    [
      { palette: { black_y: 0.01, white_y: 0.6, gamma: "1" } },
      '"palette.gamma" must be a number',
    ],
    [{ config: 3 }, '"config" must be an object'],
    [
      { config: { display_orientation: "upside-down" } },
      '"config.display_orientation" must be "landscape" or "portrait"',
    ],
    [
      { processing: {}, system_info: "e1004" },
      '"system_info" must be an object',
    ],
    [
      { processing: {}, system_info: { width: "800", height: 480 } },
      "must be positive integers",
    ],
    [
      { processing: {}, system_info: { width: 800, height: 0 } },
      "must be positive integers",
    ],
    [
      { processing: {}, system_info: { width: 800, height: 480.5 } },
      "must be positive integers",
    ],
    [
      {
        processing: {},
        system_info: { width: 800, height: 480, display_type: 16 },
      },
      '"system_info.display_type" must be a string',
    ],
  ])("rejects %j", (data, message) => {
    expect(() => parseDeviceConfig(data)).toThrow(message);
  });
});

describe("loadDeviceConfig", () => {
  let tmpDir;
  beforeAll(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "photoframe-devcfg-"));
  });
  afterAll(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  test("reads an export from disk", () => {
    expect(loadDeviceConfig(NEW_STYLE).width).toBe(1600);
  });

  test("names the file on a missing path", () => {
    const missing = path.join(tmpDir, "nope.json");
    expect(() => loadDeviceConfig(missing)).toThrow(
      `Cannot read device config ${missing}`,
    );
  });

  test("names the file on invalid JSON", () => {
    const bad = path.join(tmpDir, "bad.json");
    fs.writeFileSync(bad, "{ not json");
    expect(() => loadDeviceConfig(bad)).toThrow(
      `Device config ${bad} is not valid JSON`,
    );
  });

  test("names the file on a validation error", () => {
    const wrong = path.join(tmpDir, "wrong.json");
    fs.writeFileSync(wrong, JSON.stringify({ unrelated: true }));
    expect(() => loadDeviceConfig(wrong)).toThrow(
      `Invalid device config ${wrong}: not a device config export`,
    );
  });
});

describe("palette from an export matches the live --device-parameters path", () => {
  // --device-parameters hands the body of GET /api/settings/palette to
  // buildPalette(); the export's "palette" block is that same body.
  test("colour panel: SPECTRA6 theoretical + the file's perceived colours", () => {
    const raw = readJson(NEW_STYLE);
    const cfg = parseDeviceConfig(raw);
    const fromFile = buildPalette(cfg.palette, cfg.grayscale);
    const fromLive = buildPalette(raw.palette, false);

    expect(fromFile).toEqual(fromLive);
    expect(fromFile).toEqual({
      theoretical: SPECTRA6.theoretical,
      perceived: raw.palette,
    });
    expect(fromFile.perceived).not.toEqual(SPECTRA6.perceived);
  });

  test("grayscale panel: ramp derived from the file's luminance endpoints", () => {
    const raw = readJson(GRAYSCALE);
    const cfg = parseDeviceConfig(raw);
    const fromFile = buildPalette(cfg.palette, cfg.grayscale);
    const fromLive = buildPalette(raw.palette, true);

    expect(fromFile).toEqual(fromLive);
    expect(fromFile).toEqual(
      makeGrayscale16({ blackY: 0.012, whiteY: 0.62, gamma: 1.4 }),
    );
    expect(fromFile.perceived.grays).toHaveLength(16);
  });

  test("no device palette: library defaults", () => {
    expect(buildPalette(null, false)).toBe(SPECTRA6);
    expect(buildPalette(null, true).perceived.grays).toHaveLength(16);
  });
});

describe("--serve with a grayscale export", () => {
  // The grayscale flag --device-config derives from the file has to reach
  // the serve pipeline too, or the GC16 calibration palette is dithered as
  // six colours and /image fails.
  const port = 9003;
  const width = 200;
  const height = 120;
  let server;

  beforeAll(async () => {
    const cfg = parseDeviceConfig(readJson(GRAYSCALE));
    server = await createImageServer(
      path.join(__dirname, "test-albums"),
      port,
      "epdgz",
      cfg.palette,
      {
        ...cfg.processing,
        grayscale: cfg.grayscale,
        displayWidth: width,
        displayHeight: height,
      },
      { silent: true },
    );
  }, 30000);

  afterAll(async () => {
    if (server) await new Promise((resolve) => server.close(resolve));
  });

  test("serves a grayscale epdgz and its thumbnail", async () => {
    const res = await fetch(`http://localhost:${port}/image`);
    expect(res.status).toBe(200);
    expect(res.headers.get("content-type")).toBe("application/octet-stream");
    const body = Buffer.from(await res.arrayBuffer());
    // 4 bits per pixel, gzip-compressed
    expect(gunzipSync(body)).toHaveLength(Math.ceil((width * height) / 2));

    const thumbUrl = res.headers.get("x-thumbnail-url");
    expect(thumbUrl).toBeTruthy();
    const thumb = await fetch(thumbUrl);
    expect(thumb.status).toBe(200);
    expect(thumb.headers.get("content-type")).toBe("image/jpeg");
  });
});

describe("photoframe-process --device-config", () => {
  let outDir;
  beforeAll(() => {
    outDir = fs.mkdtempSync(path.join(os.tmpdir(), "photoframe-cli-"));
  });
  afterAll(() => {
    fs.rmSync(outDir, { recursive: true, force: true });
  });

  test("sizes the output from the file's system_info and uses its settings", async () => {
    const result = runCli(
      [IMAGE, "--device-config", NEW_STYLE, "--format", "png", "-o", outDir],
      outDir,
    );
    expect(result.status).toBe(0);
    expect(result.stdout).toContain("Using device config");
    expect(result.stdout).toContain("display=1600x1200");
    expect(result.stdout).toContain("orientation=portrait");
    expect(result.stdout).toContain("tone_mode=scurve");
    expect(result.stdout).toContain("dither_algorithm=stucki");

    const img = await loadImage(path.join(outDir, "landscape.png"));
    expect(img.width).toBe(1600);
    expect(img.height).toBe(1200);
    expect(fs.existsSync(path.join(outDir, "landscape.jpg"))).toBe(true);
  }, 120000);

  test("old-style export needs -d for the display size", () => {
    const result = runCli(
      [IMAGE, "--device-config", OLD_STYLE, "--format", "png", "-o", outDir],
      outDir,
    );
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("has no system_info");
    expect(result.stderr).toContain("-d WxH");
  });

  test("old-style export: one of --display-width/--display-height is not a size", () => {
    const result = runCli(
      [IMAGE, "--device-config", OLD_STYLE, "--display-width", "1600"],
      outDir,
    );
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("has no system_info");
  });

  test("old-style export: --host whose query fails does not fall back to 800x480", () => {
    // Port 1 refuses the connection at once
    const result = runCli(
      [IMAGE, "--device-config", OLD_STYLE, "--host", "127.0.0.1:1"],
      outDir,
    );
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("Could not fetch system info");
    expect(result.stderr).toContain("the device could not be queried");
    expect(result.stderr).not.toContain("Using default resolution");
  });

  test("old-style export with -d processes at that size", async () => {
    const out = path.join(outDir, "old");
    const result = runCli(
      [
        IMAGE,
        "--device-config",
        OLD_STYLE,
        "-d",
        "400x240",
        "--format",
        "png",
        "-o",
        out,
      ],
      outDir,
    );
    expect(result.status).toBe(0);
    const img = await loadImage(path.join(out, "landscape.png"));
    expect(img.width).toBe(400);
    expect(img.height).toBe(240);
  });

  test("rejects --device-config together with --device-parameters", () => {
    const result = runCli(
      [IMAGE, "--device-config", NEW_STYLE, "--device-parameters"],
      outDir,
    );
    expect(result.status).toBe(1);
    expect(result.stderr).toContain(
      "--device-config and --device-parameters cannot be used together",
    );
  });

  test("reports an unreadable or invalid file and exits", () => {
    const result = runCli(
      [IMAGE, "--device-config", path.join(outDir, "missing.json")],
      outDir,
    );
    expect(result.status).toBe(1);
    expect(result.stderr).toContain("Cannot read device config");
  });
});
