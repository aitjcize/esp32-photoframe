#!/usr/bin/env node

import { Command } from "commander";
import fs from "fs";
import path from "path";
import os from "os";
import http from "http";
import { fileURLToPath } from "url";
import { createCanvas } from "canvas";
import FormData from "form-data";
import {
  generateThumbnail,
  createPNG,
  getPreset,
  getPresetNames,
  getDefaultParams,
} from "@aitjcize/epaper-image-convert";
import { processImagePipeline } from "./utils.js";
import { createImageServer } from "./server.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Thumbnail dimensions (half of display resolution)
const THUMBNAIL_WIDTH = 400;
const THUMBNAIL_HEIGHT = 240;

// Get default parameters from the library
const DEFAULT_PARAMS = {
  ...getDefaultParams(),
};

// Fetch processing settings from device
async function fetchDeviceSettings(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/settings/processing`;
    console.log(`Fetching settings from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const settings = JSON.parse(data);
              console.log("Device settings loaded successfully");
              console.log(
                `  exposure=${settings.exposure}, saturation=${settings.saturation}, tone_mode=${settings.toneMode}`,
              );
              if (settings.toneMode === "scurve") {
                console.log(
                  `  scurve: strength=${settings.strength}, shadow=${settings.shadowBoost}, highlight=${settings.highlightCompress}, midpoint=${settings.midpoint}`,
                );
              } else if (settings.toneMode === "contrast") {
                console.log(`  contrast=${settings.contrast}`);
              }
              console.log(
                `  processing_mode=${settings.processingMode}, dither_algorithm=${settings.ditherAlgorithm || "floyd-steinberg"}`,
              );
              resolve(settings);
            } catch (error) {
              reject(
                new Error(`Failed to parse device settings: ${error.message}`),
              );
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Failed to fetch settings from device`,
              ),
            );
          }
        });
      })
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Fetch color palette from device
async function fetchDevicePalette(host) {
  return new Promise((resolve, reject) => {
    const url = `http://${host}/api/settings/palette`;
    console.log(`Fetching color palette from device: ${url}`);

    http
      .get(url, (res) => {
        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const palette = JSON.parse(data);
              console.log("Device color palette loaded successfully");
              resolve(palette);
            } catch (error) {
              reject(
                new Error(`Failed to parse device palette: ${error.message}`),
              );
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Failed to fetch palette from device`,
              ),
            );
          }
        });
      })
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Display image directly on device without saving (via /api/display-image)
async function displayDirectly(host, pngPath, thumbPath, retries = 3) {
  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      if (attempt > 1) {
        console.log(
          `  Retry attempt ${attempt}/${retries} after 5 second delay...`,
        );
        await new Promise((resolve) => setTimeout(resolve, 5000));
      }

      await displayDirectlyOnce(host, pngPath, thumbPath);
      return; // Success, exit retry loop
    } catch (error) {
      if (attempt === retries) {
        throw error; // Last attempt failed, throw error
      }
      console.log(`  Display failed: ${error.message}`);
    }
  }
}

// Single direct display attempt
function displayDirectlyOnce(host, pngPath, thumbPath) {
  return new Promise((resolve, reject) => {
    console.log(`Displaying image directly on device: ${host}`);
    console.log(`  Image: ${pngPath}`);
    console.log(`  Thumbnail: ${thumbPath}`);

    const form = new FormData();
    form.append("image", fs.createReadStream(pngPath), {
      filename: path.basename(pngPath),
      contentType: "image/png",
    });
    form.append("thumbnail", fs.createReadStream(thumbPath), {
      filename: path.basename(thumbPath),
      contentType: "image/jpeg",
    });

    // Use form.submit() which properly handles Content-Length
    form
      .submit(
        {
          protocol: "http:",
          host: host,
          port: 80,
          path: "/api/display-image",
          method: "POST",
        },
        (err, res) => {
          if (err) {
            reject(new Error(`Failed to submit form: ${err.message}`));
            return;
          }

          let data = "";

          res.on("data", (chunk) => {
            data += chunk;
          });

          res.on("end", () => {
            if (res.statusCode === 200) {
              console.log(`✓ Image displayed successfully`);
              resolve();
            } else {
              reject(
                new Error(`Server returned status ${res.statusCode}: ${data}`),
              );
            }
          });

          res.on("error", (error) => {
            reject(new Error(`Failed to read response: ${error.message}`));
          });
        },
      )
      .on("error", (error) => {
        reject(
          new Error(`Failed to connect to device at ${host}: ${error.message}`),
        );
      });
  });
}

// Upload PNG and thumbnail to device with retry logic
async function uploadToDevice(
  host,
  pngPath,
  thumbPath,
  album = null,
  retries = 3,
) {
  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      if (attempt > 1) {
        console.log(
          `  Retry attempt ${attempt}/${retries} after 5 second delay...`,
        );
        await new Promise((resolve) => setTimeout(resolve, 5000));
      }

      await uploadToDeviceOnce(host, pngPath, thumbPath, album);
      return; // Success, exit retry loop
    } catch (error) {
      if (attempt === retries) {
        throw error; // Last attempt failed, throw error
      }
      console.log(`  Upload failed: ${error.message}`);
    }
  }
}

// Single upload attempt
function uploadToDeviceOnce(host, pngPath, thumbPath, album = null) {
  return new Promise((resolve, reject) => {
    console.log(`Uploading to device: ${host}`);
    console.log(`  Image: ${pngPath}`);
    console.log(`  Thumbnail: ${thumbPath}`);
    if (album) {
      console.log(`  Album: ${album}`);
    }

    const form = new FormData();
    form.append("image", fs.createReadStream(pngPath), {
      filename: path.basename(pngPath),
      contentType: "image/png",
    });
    form.append("thumbnail", fs.createReadStream(thumbPath), {
      filename: path.basename(thumbPath),
      contentType: "image/jpeg",
    });

    // Build path with album query parameter if provided
    let uploadPath = "/api/upload";
    if (album) {
      uploadPath += `?album=${encodeURIComponent(album)}`;
    }

    // Use form.submit() which properly handles Content-Length
    form.submit(
      {
        protocol: "http:",
        host: host,
        port: 80,
        path: uploadPath,
        method: "POST",
      },
      (err, res) => {
        if (err) {
          reject(new Error(`Failed to submit form: ${err.message}`));
          return;
        }

        let data = "";

        res.on("data", (chunk) => {
          data += chunk;
        });

        res.on("end", () => {
          if (res.statusCode === 200) {
            try {
              const response = JSON.parse(data);
              console.log(`✓ Upload successful: ${response.filepath}`);
              resolve(response);
            } catch (error) {
              reject(new Error(`Failed to parse response: ${error.message}`));
            }
          } else {
            reject(
              new Error(
                `HTTP ${res.statusCode}: Upload failed - ${data || "Unknown error"}`,
              ),
            );
          }
        });

        res.on("error", (error) => {
          reject(new Error(`Response error: ${error.message}`));
        });
      },
    );
  });
}

// BMP file writing (24-bit RGB format)
function writeBMP(imageData, outputPath) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  // BMP header for 24-bit RGB
  const fileHeaderSize = 14;
  const infoHeaderSize = 40;
  const headerSize = fileHeaderSize + infoHeaderSize;
  const rowSize = Math.floor((width * 3 + 3) / 4) * 4; // 3 bytes per pixel, padded to multiple of 4
  const imageSize = rowSize * height;
  const fileSize = headerSize + imageSize;

  const buffer = Buffer.alloc(fileSize);
  let offset = 0;

  // File header (14 bytes)
  buffer.write("BM", offset);
  offset += 2;
  buffer.writeUInt32LE(fileSize, offset);
  offset += 4;
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Reserved
  buffer.writeUInt32LE(headerSize, offset);
  offset += 4;

  // Info header (40 bytes)
  buffer.writeUInt32LE(infoHeaderSize, offset);
  offset += 4;
  buffer.writeInt32LE(width, offset);
  offset += 4;
  buffer.writeInt32LE(height, offset);
  offset += 4;
  buffer.writeUInt16LE(1, offset);
  offset += 2; // Planes
  buffer.writeUInt16LE(24, offset);
  offset += 2; // Bits per pixel (24-bit RGB)
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Compression (none)
  buffer.writeUInt32LE(imageSize, offset);
  offset += 4;
  buffer.writeInt32LE(2835, offset);
  offset += 4; // X pixels per meter
  buffer.writeInt32LE(2835, offset);
  offset += 4; // Y pixels per meter
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Colors used (0 = all colors)
  buffer.writeUInt32LE(0, offset);
  offset += 4; // Important colors

  // Pixel data (bottom-up, left-to-right, BGR format)
  for (let y = height - 1; y >= 0; y--) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      const r = data[idx];
      const g = data[idx + 1];
      const b = data[idx + 2];

      // Write BGR (BMP format)
      buffer.writeUInt8(b, offset++);
      buffer.writeUInt8(g, offset++);
      buffer.writeUInt8(r, offset++);
    }

    // Padding to make row size multiple of 4
    const padding = rowSize - width * 3;
    for (let i = 0; i < padding; i++) {
      buffer.writeUInt8(0, offset++);
    }
  }

  fs.writeFileSync(outputPath, buffer);
}

// Check if file is an image
function isImageFile(filename) {
  const ext = path.extname(filename).toLowerCase();
  return [
    ".jpg",
    ".jpeg",
    ".png",
    ".gif",
    ".bmp",
    ".webp",
    ".heic",
    ".heif",
  ].includes(ext);
}

// Process all images in a folder structure (albums)
async function processFolderStructure(
  inputDir,
  outputDir,
  options,
  devicePalette,
  uploadHost = null,
) {
  console.log(`\nProcessing folder structure: ${inputDir}`);
  console.log(`Output directory: ${outputDir}\n`);

  // Read all subdirectories (albums)
  const entries = fs.readdirSync(inputDir, { withFileTypes: true });
  const albums = entries.filter((entry) => entry.isDirectory());

  if (albums.length === 0) {
    console.log("No subdirectories (albums) found in input directory.");
    return;
  }

  console.log(
    `Found ${albums.length} album(s): ${albums.map((a) => a.name).join(", ")}\n`,
  );

  let totalProcessed = 0;
  let totalUploaded = 0;
  let totalErrors = 0;

  for (const album of albums) {
    const albumName = album.name;
    const albumInputPath = path.join(inputDir, albumName);
    const albumOutputPath = path.join(outputDir, albumName);

    console.log(`\n=== Processing album: ${albumName} ===`);

    // Create output directory for this album (always create, even if tmpdir)
    fs.mkdirSync(albumOutputPath, { recursive: true });

    // Get all image files in this album
    const files = fs.readdirSync(albumInputPath);
    const imageFiles = files.filter(isImageFile);

    if (imageFiles.length === 0) {
      console.log(`  No images found in album: ${albumName}`);
      continue;
    }

    console.log(`  Found ${imageFiles.length} image(s)`);

    // Process each image
    for (let i = 0; i < imageFiles.length; i++) {
      const imageFile = imageFiles[i];
      const inputPath = path.join(albumInputPath, imageFile);
      const baseName = path.basename(imageFile, path.extname(imageFile));
      const outputPng = path.join(albumOutputPath, `${baseName}.png`);
      const outputThumb = path.join(albumOutputPath, `${baseName}.jpg`);

      try {
        console.log(
          `  [${i + 1}/${imageFiles.length}] Processing: ${imageFile}`,
        );
        await processImageFile(
          inputPath,
          outputPng,
          outputThumb,
          options,
          devicePalette,
        );
        totalProcessed++;

        // Upload if requested
        if (uploadHost) {
          try {
            await uploadToDevice(uploadHost, outputPng, outputThumb, albumName);
            totalUploaded++;
          } catch (error) {
            console.error(`  ERROR uploading ${imageFile}: ${error.message}`);
            totalErrors++;
          }
        }
      } catch (error) {
        console.error(`  ERROR processing ${imageFile}: ${error.message}`);
        totalErrors++;
      }
    }
  }

  console.log(`\n=== Summary ===`);
  console.log(`Total images processed: ${totalProcessed}`);
  if (uploadHost) {
    console.log(`Total images uploaded: ${totalUploaded}`);
  }
  if (totalErrors > 0) {
    console.log(`Total errors: ${totalErrors}`);
  }
  console.log(`Output directory: ${outputDir}`);
}

async function processImageFile(
  inputPath,
  outputBmp,
  outputThumb,
  processingOptions,
  devicePalette = null,
) {
  console.log(`Processing: ${inputPath}`);

  // Use shared processing pipeline with verbose logging (library handles parameter logging)
  // Skip rotation when rendering measured palette for easier preview viewing
  const { canvas, originalCanvas } = await processImagePipeline(
    inputPath,
    processingOptions,
    processingOptions.displayWidth,
    processingOptions.displayHeight,
    devicePalette,
    {
      verbose: processingOptions.verbose || true,
      skipRotation: processingOptions.renderMeasured,
    },
  );

  const ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);

  // 5. Write output file (BMP or PNG)
  const format = processingOptions.format || "png";
  if (format === "png") {
    const outputPng = outputBmp.replace(/\.bmp$/, ".png");
    console.log(`  Writing PNG: ${outputPng}`);
    const pngBuffer = await createPNG(canvas);
    fs.writeFileSync(outputPng, pngBuffer);
  } else if (format === "bmp") {
    console.log(`  Writing BMP: ${outputBmp}`);
    writeBMP(imageData, outputBmp);
  } else {
    throw new Error(`Unsupported format: ${format}. Use 'png' or 'bmp'`);
  }

  // 6. Generate thumbnail if requested (from EXIF-corrected source, not processed image)
  if (processingOptions.generateThumbnail && outputThumb) {
    console.log(`  Generating thumbnail: ${outputThumb}`);

    // Use shared thumbnail generation function
    const thumbCanvas = generateThumbnail(
      originalCanvas,
      THUMBNAIL_WIDTH,
      THUMBNAIL_HEIGHT,
      createCanvas,
    );

    const buffer = thumbCanvas.toBuffer("image/jpeg", { quality: 0.8 });
    fs.writeFileSync(outputThumb, buffer);
  }

  console.log(`Done!`);
}

// CLI setup
const program = new Command();

program
  .name("photoframe-process")
  .description("ESP32 PhotoFrame image processing CLI")
  .version("1.0.0")
  .argument(
    "<input>",
    "Input image file or directory with album subdirectories",
  )
  .option("-o, --output-dir <dir>", "Output directory", ".")
  .option(
    "--suffix <suffix>",
    "Suffix to add to output filename (single file mode only)",
    "",
  )
  .option("-v, --verbose", "Enable verbose logging")
  .option("--format <format>", "Output format: png or bmp", "png")
  .option(
    "--preset <name>",
    `Processing preset: ${getPresetNames().join(", ")} `,
    "balanced",
  )
  .option(
    "--upload",
    "Upload converted PNG and thumbnail to device (requires --host)",
  )
  .option(
    "--direct",
    "Display image directly on device without saving (requires --host, single file only)",
  )
  .option(
    "--serve",
    "Start HTTP server to serve images from album directory structure",
  )
  .option("--serve-port <port>", "Port for HTTP server in --serve mode", "8080")
  .option(
    "--serve-format <format>",
    "Image format to serve: png, jpg, or bmp (default: png)",
    "png",
  )
  .option(
    "--host <host>",
    "Device hostname or IP address (default: photoframe.local)",
    "photoframe.local",
  )
  .option("--device-parameters", "Fetch processing parameters from device")
  .option(
    "--exposure <value>",
    "Exposure multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option(
    "--saturation <value>",
    "Saturation multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option("--tone-mode <mode>", "Tone mapping mode: scurve or contrast")
  .option(
    "--contrast <value>",
    "Contrast multiplier for simple mode (0.5-2.0, 1.0=normal)",
    parseFloat,
  )
  .option(
    "--scurve-strength <value>",
    "S-curve overall strength (0.0-1.0)",
    parseFloat,
  )
  .option(
    "--scurve-shadow <value>",
    "S-curve shadow boost (0.0-1.0)",
    parseFloat,
  )
  .option(
    "--scurve-highlight <value>",
    "S-curve highlight compress (0.5-5.0)",
    parseFloat,
  )
  .option("--scurve-midpoint <value>", "S-curve midpoint (0.3-0.7)", parseFloat)
  .option("--color-method <method>", "Color matching: rgb or lab")
  .option(
    "--render-measured",
    "Render BMP with measured palette colors (darker output for preview)",
  )
  .option(
    "--dither-algorithm <algorithm>",
    "Dithering algorithm: floyd-steinberg, stucki, burkes, or sierra",
  )
  .option("--display-width <width>", "Display width in pixels", parseInt, 800)
  .option(
    "--display-height <height>",
    "Display height in pixels",
    parseInt,
    480,
  )
  .option(
    "-d, --dimension <WxH>",
    "Display dimension (e.g., 800x480) - overrides display-width/height",
  )
  .option("--compress-dynamic-range", "Compress dynamic range to display range")
  .option("--no-compress-dynamic-range", "Disable dynamic range compression")
  .action(async (input, options) => {
    let outputDir;
    let useTmpDir = false;

    // Fetch device settings if --device-parameters is specified
    let deviceSettings = null;
    let devicePalette = null;
    if (options.deviceParameters) {
      try {
        deviceSettings = await fetchDeviceSettings(options.host);
        devicePalette = await fetchDevicePalette(options.host);
      } catch (error) {
        console.error(`Error: ${error.message}`);
        process.exit(1);
      }
    }

    try {
      const inputPath = path.resolve(input);
      if (!fs.existsSync(inputPath)) {
        console.error(`Error: Input path not found: ${inputPath}`);
        process.exit(1);
      }

      // Parse dimension if provided
      if (options.dimension) {
        const match = options.dimension.match(/^(\d+)x(\d+)$/);
        if (match) {
          options.displayWidth = parseInt(match[1]);
          options.displayHeight = parseInt(match[2]);
        } else {
          console.error(
            `Error: Invalid dimension format "${options.dimension}". Use WxH (e.g. 800x480)`,
          );
          process.exit(1);
        }
      }

      // Apply preset values if not overridden by explicit options
      const presetName = options.preset || "balanced";
      // presetParams is declared here, ensure no duplicate declaration below
      const presetParams = getPreset(presetName) || {};

      // Helper to set option if not defined, strictly checking undefined
      // so that 0 is treated as a valid value
      const setIfNotDefined = (key, value) => {
        if (options[key] === undefined && value !== undefined) {
          options[key] = value;
        }
      };

      // Set defaults from preset
      setIfNotDefined("exposure", presetParams.exposure);
      setIfNotDefined("saturation", presetParams.saturation);
      setIfNotDefined("toneMode", presetParams.toneMode);
      setIfNotDefined("contrast", presetParams.contrast);
      setIfNotDefined("scurveStrength", presetParams.strength);
      setIfNotDefined("scurveShadow", presetParams.shadowBoost);
      setIfNotDefined("scurveHighlight", presetParams.highlightCompress);
      setIfNotDefined("scurveMidpoint", presetParams.midpoint);
      setIfNotDefined("colorMethod", presetParams.colorMethod);
      setIfNotDefined("ditherAlgorithm", presetParams.ditherAlgorithm);
      setIfNotDefined(
        "compressDynamicRange",
        presetParams.compressDynamicRange,
      );

      // Set fallback defaults if still undefined (from global defaults)
      const libraryDefaults = getDefaultParams();
      setIfNotDefined("exposure", libraryDefaults.exposure);
      setIfNotDefined("saturation", libraryDefaults.saturation);
      setIfNotDefined("toneMode", libraryDefaults.toneMode);
      setIfNotDefined("contrast", libraryDefaults.contrast);
      setIfNotDefined("scurveStrength", libraryDefaults.strength);
      setIfNotDefined("scurveShadow", libraryDefaults.shadowBoost);
      setIfNotDefined("scurveHighlight", libraryDefaults.highlightCompress);
      setIfNotDefined("scurveMidpoint", libraryDefaults.midpoint);
      setIfNotDefined("colorMethod", libraryDefaults.colorMethod);
      setIfNotDefined("ditherAlgorithm", libraryDefaults.ditherAlgorithm);
      setIfNotDefined(
        "compressDynamicRange",
        libraryDefaults.compressDynamicRange,
      );

      if (!options.silent) {
        console.log(`Using preset: ${presetName}`);
      }

      // Build processing options with priority: device settings > CLI options > preset > defaults
      // When using device settings, they take full priority
      // Otherwise: user CLI options override preset values, preset values override defaults
      const processOptions = deviceSettings
        ? {
            generateThumbnail: true,
            exposure: deviceSettings.exposure,
            saturation: deviceSettings.saturation,
            toneMode: deviceSettings.toneMode,
            contrast: deviceSettings.contrast,
            scurveStrength: deviceSettings.strength,
            scurveShadow: deviceSettings.shadowBoost,
            scurveHighlight: deviceSettings.highlightCompress,
            scurveMidpoint: deviceSettings.midpoint,
            colorMethod: deviceSettings.colorMethod,
            renderMeasured: options.renderMeasured || false, // Not in device settings usually
            ditherAlgorithm:
              deviceSettings.ditherAlgorithm || options.ditherAlgorithm,
            compressDynamicRange: deviceSettings.compressDynamicRange,
            displayWidth: options.displayWidth,
            displayHeight: options.displayHeight,
            format: options.format,
          }
        : {
            generateThumbnail: true,
            // User CLI options override preset values
            exposure:
              options.exposure ??
              presetParams?.exposure ??
              DEFAULT_PARAMS.exposure,
            saturation:
              options.saturation ??
              presetParams?.saturation ??
              DEFAULT_PARAMS.saturation,
            toneMode:
              options.toneMode ??
              presetParams?.toneMode ??
              DEFAULT_PARAMS.toneMode,
            contrast:
              options.contrast ??
              presetParams?.contrast ??
              DEFAULT_PARAMS.contrast,
            scurveStrength:
              options.scurveStrength ??
              presetParams?.strength ??
              DEFAULT_PARAMS.strength,
            scurveShadow:
              options.scurveShadow ??
              presetParams?.shadowBoost ??
              DEFAULT_PARAMS.shadowBoost,
            scurveHighlight:
              options.scurveHighlight ??
              presetParams?.highlightCompress ??
              DEFAULT_PARAMS.highlightCompress,
            scurveMidpoint:
              options.scurveMidpoint ??
              presetParams?.midpoint ??
              DEFAULT_PARAMS.midpoint,
            colorMethod:
              options.colorMethod ??
              presetParams?.colorMethod ??
              DEFAULT_PARAMS.colorMethod,
            renderMeasured: options.renderMeasured ?? false,
            ditherAlgorithm:
              options.ditherAlgorithm ??
              presetParams?.ditherAlgorithm ??
              DEFAULT_PARAMS.ditherAlgorithm,
            compressDynamicRange:
              options.compressDynamicRange ??
              presetParams?.compressDynamicRange ??
              DEFAULT_PARAMS.compressDynamicRange,
            displayWidth: options.displayWidth,
            displayHeight: options.displayHeight,
            format: options.format,
          };

      // Check if --serve mode is enabled
      if (options.serve) {
        const inputStats = fs.statSync(inputPath);
        if (!inputStats.isDirectory()) {
          console.error(
            "Error: --serve mode requires a directory with album structure",
          );
          process.exit(1);
        }

        const port = parseInt(options.servePort);
        if (isNaN(port) || port < 1 || port > 65535) {
          console.error(`Error: Invalid port number: ${options.servePort}`);
          process.exit(1);
        }

        // Start HTTP server and keep running
        await createImageServer(
          inputPath,
          port,
          options.serveFormat,
          devicePalette,
          processOptions, // Pass resolved options
          options,
        );
        return; // Server runs indefinitely
      }

      // Validate --direct and --upload are mutually exclusive
      if (options.direct && options.upload) {
        console.error("Error: --direct and --upload cannot be used together");
        process.exit(1);
      }

      // Check input type and validate --direct option
      const inputStats = fs.statSync(inputPath);
      const isDirectory = inputStats.isDirectory();

      // Validate --direct requires single file
      if (options.direct && isDirectory) {
        console.error(
          "Error: --direct option requires a single file input, not a directory",
        );
        process.exit(1);
      }

      if (options.upload || options.direct) {
        outputDir = fs.mkdtempSync(path.join(os.tmpdir(), "photoframe-"));
        useTmpDir = true;
        console.log(`Using temporary directory: ${outputDir}`);
      } else {
        outputDir = path.resolve(options.outputDir);
        if (!fs.existsSync(outputDir)) {
          fs.mkdirSync(outputDir, { recursive: true });
        }
      }

      // Explicit preset option validation
      if (options.preset) {
        if (!getPreset(options.preset)) {
          console.error(`Error: Unknown preset "${options.preset}"`);
          console.error(`Available presets: ${getPresetNames().join(", ")}`);
          process.exit(1);
        }
      }

      // Process based on input type
      if (isDirectory) {
        // Process folder structure (albums)
        await processFolderStructure(
          inputPath,
          outputDir,
          processOptions,
          devicePalette,
          options.upload ? options.host : null,
        );
      } else {
        // Process single file
        const baseName = path.basename(input, path.extname(input));
        const suffix = options.suffix || "";
        const format = options.format || "png";
        const ext = format === "bmp" ? ".bmp" : ".png";
        const outputFile = path.join(outputDir, `${baseName}${suffix}${ext}`);
        const outputThumb = path.join(outputDir, `${baseName}${suffix}.jpg`);

        await processImageFile(
          inputPath,
          outputFile,
          outputThumb,
          processOptions,
          devicePalette,
        );

        // Upload or display directly on device (only works with PNG)
        if (options.upload || options.direct) {
          if (format !== "png") {
            console.error(`Error: Upload/direct display requires PNG format`);
            process.exit(1);
          }
          if (!fs.existsSync(outputFile)) {
            console.error(`Error: PNG file not found: ${outputFile}`);
            process.exit(1);
          }
          if (!fs.existsSync(outputThumb)) {
            console.error(`Error: Thumbnail file not found: ${outputThumb}`);
            process.exit(1);
          }

          try {
            if (options.direct) {
              await displayDirectly(options.host, outputFile, outputThumb);
            } else {
              await uploadToDevice(options.host, outputFile, outputThumb, null);
            }
          } catch (error) {
            const action = options.direct ? "Display" : "Upload";
            console.error(`${action} failed: ${error.message}`);
            process.exit(1);
          }
        }
      }

      // Cleanup temporary directory if used
      if (useTmpDir) {
        console.log(`\nCleaning up temporary directory: ${outputDir}`);
        try {
          fs.rmSync(outputDir, { recursive: true, force: true });
          console.log(`✓ Temporary files cleaned up`);
        } catch (error) {
          console.warn(
            `Warning: Failed to cleanup temporary directory: ${error.message}`,
          );
        }
      }
    } catch (error) {
      console.error(`Error processing: ${error.message}`);
      console.error(error.stack);

      // Cleanup temporary directory on error if used
      if (useTmpDir && outputDir) {
        try {
          fs.rmSync(outputDir, { recursive: true, force: true });
        } catch (cleanupError) {
          // Ignore cleanup errors on error path
        }
      }

      process.exit(1);
    }
  });

program.parse();
