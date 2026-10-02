/**
 * Device config files exported by the frame's web UI
 * (Settings -> Maintenance -> Config Backup -> Export Config).
 *
 * The export is a JSON object with the raw responses of the device's REST
 * endpoints, so each block has exactly the shape --device-parameters fetches
 * live:
 *
 *   {
 *     "config":      GET /api/config               (display_orientation, ...)
 *     "processing":  GET /api/settings/processing  (exposure, toneMode, ...)
 *     "palette":     GET /api/settings/palette     (perceived colours, or the
 *                                                   gray calibration on GC16)
 *     "system_info": { board_name, display_type, width, height, version }
 *   }
 *
 * `system_info` was added later; exports from older firmware don't have it,
 * and the caller then needs the display size from -d.
 */

import fs from "fs";

const PALETTE_COLORS = ["black", "white", "yellow", "red", "blue", "green"];

function isPlainObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function isPositiveInteger(value) {
  return Number.isInteger(value) && value > 0;
}

/**
 * A grayscale (GC16) device reports its palette as the two measured
 * luminance endpoints instead of six colours.
 */
export function isGrayCalibration(palette) {
  return (
    isPlainObject(palette) &&
    typeof palette.black_y === "number" &&
    typeof palette.white_y === "number"
  );
}

function validatePalette(palette) {
  if (!isPlainObject(palette)) {
    throw new Error('"palette" must be an object');
  }
  if ("black_y" in palette || "white_y" in palette) {
    if (!isGrayCalibration(palette)) {
      throw new Error(
        '"palette.black_y" and "palette.white_y" must be numbers',
      );
    }
    if ("gamma" in palette && typeof palette.gamma !== "number") {
      throw new Error('"palette.gamma" must be a number');
    }
    return;
  }
  for (const name of PALETTE_COLORS) {
    const color = palette[name];
    if (
      !isPlainObject(color) ||
      !["r", "g", "b"].every((c) => typeof color[c] === "number")
    ) {
      throw new Error(
        `"palette.${name}" must be an object with numeric r, g and b`,
      );
    }
  }
}

function validateSystemInfo(info) {
  if (!isPlainObject(info)) {
    throw new Error('"system_info" must be an object');
  }
  if (!isPositiveInteger(info.width) || !isPositiveInteger(info.height)) {
    throw new Error(
      '"system_info.width" and "system_info.height" must be positive integers',
    );
  }
  if (
    info.display_type !== undefined &&
    typeof info.display_type !== "string"
  ) {
    throw new Error('"system_info.display_type" must be a string');
  }
}

/**
 * Validate a parsed device config export and pull out what the CLI needs.
 *
 * @param {*} data - Parsed JSON of the export
 * @returns {{
 *   processing: Object|null,
 *   palette: Object|null,
 *   orientation: string|null,
 *   width: number|null,
 *   height: number|null,
 *   version: string,
 *   grayscale: boolean,
 *   boardName: string|null,
 * }}
 * @throws {Error} If the data is not a device config export
 */
export function parseDeviceConfig(data) {
  if (!isPlainObject(data)) {
    throw new Error("expected a JSON object");
  }
  const { config, processing, palette, system_info: systemInfo } = data;
  if (
    processing === undefined &&
    palette === undefined &&
    config === undefined
  ) {
    throw new Error(
      'not a device config export (no "processing", "palette" or "config" key)',
    );
  }
  if (processing !== undefined && !isPlainObject(processing)) {
    throw new Error('"processing" must be an object');
  }
  if (palette !== undefined) {
    validatePalette(palette);
  }
  if (config !== undefined && !isPlainObject(config)) {
    throw new Error('"config" must be an object');
  }
  if (systemInfo !== undefined) {
    validateSystemInfo(systemInfo);
  }

  let orientation = null;
  if (config && config.display_orientation !== undefined) {
    if (!["landscape", "portrait"].includes(config.display_orientation)) {
      throw new Error(
        '"config.display_orientation" must be "landscape" or "portrait"',
      );
    }
    orientation = config.display_orientation;
  }

  // Same rule as the webapp's appStore.isGrayscale ("gc16", future "gc8"...).
  // Without system_info the palette shape still tells: only a grayscale
  // panel reports luminance endpoints.
  const grayscale = systemInfo
    ? (systemInfo.display_type || "").startsWith("gc")
    : isGrayCalibration(palette);

  return {
    processing: processing ?? null,
    palette: palette ?? null,
    orientation,
    width: systemInfo ? systemInfo.width : null,
    height: systemInfo ? systemInfo.height : null,
    version: (systemInfo && systemInfo.version) || "",
    grayscale,
    boardName: (systemInfo && systemInfo.board_name) || null,
  };
}

/**
 * Read and validate a device config export from disk.
 *
 * @param {string} filePath
 * @returns {ReturnType<typeof parseDeviceConfig>}
 * @throws {Error} With the file name in the message
 */
export function loadDeviceConfig(filePath) {
  let text;
  try {
    text = fs.readFileSync(filePath, "utf8");
  } catch (error) {
    throw new Error(`Cannot read device config ${filePath}: ${error.message}`);
  }
  let data;
  try {
    data = JSON.parse(text);
  } catch (error) {
    throw new Error(
      `Device config ${filePath} is not valid JSON: ${error.message}`,
    );
  }
  try {
    return parseDeviceConfig(data);
  } catch (error) {
    throw new Error(`Invalid device config ${filePath}: ${error.message}`);
  }
}
