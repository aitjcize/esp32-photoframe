#!/usr/bin/env node

import { createCanvas, loadImage } from "canvas";
import { Command } from "commander";
import ExifParser from "exif-parser";
import fs from "fs";
import heicConvert from "heic-convert";
import http from "http";
import path from "path";
import { fileURLToPath } from "url";
import {
  processImage,
  PALETTE_THEORETICAL,
  rotate90Clockwise,
  applyExifOrientation,
  resizeImageCover,
  generateThumbnail,
} from "./image-processor.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const DISPLAY_WIDTH = 800;
const DISPLAY_HEIGHT = 480;

// Centralized default configuration for image processing
// Keep in sync with webapp/app.js DEFAULT_PARAMS and firmware processing_settings.c
const DEFAULT_PARAMS = {
  exposure: 1.0,
  saturation: 1.3,
  toneMode: "scurve",
  contrast: 1.0,
  strength: 0.9,
  shadowBoost: 0.0,
  highlightCompress: 1.5,
  midpoint: 0.5,
  colorMethod: "rgb",
  processingMode: "enhanced",
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
  deviceSettings,
  devicePalette,
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
  let totalErrors = 0;

  for (const album of albums) {
    const albumName = album.name;
    const albumInputPath = path.join(inputDir, albumName);
    const albumOutputPath = path.join(outputDir, albumName);

    console.log(`\n=== Processing album: ${albumName} ===`);

    // Create output directory for this album
    if (!fs.existsSync(albumOutputPath)) {
      fs.mkdirSync(albumOutputPath, { recursive: true });
    }

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
      const outputBmp = path.join(albumOutputPath, `${baseName}.bmp`);
      const outputThumb = options.generateThumbnail
        ? path.join(albumOutputPath, `${baseName}.jpg`)
        : null;

      try {
        console.log(
          `  [${i + 1}/${imageFiles.length}] Processing: ${imageFile}`,
        );
        await processImageFile(
          inputPath,
          outputBmp,
          outputThumb,
          options,
          devicePalette,
        );
        totalProcessed++;
      } catch (error) {
        console.error(`  ERROR processing ${imageFile}: ${error.message}`);
        totalErrors++;
      }
    }
  }

  console.log(`\n=== Summary ===`);
  console.log(`Total images processed: ${totalProcessed}`);
  if (totalErrors > 0) {
    console.log(`Total errors: ${totalErrors}`);
  }
  console.log(`Output directory: ${outputDir}`);
}

function getExifOrientation(filePath) {
  try {
    const buffer = fs.readFileSync(filePath);
    const parser = ExifParser.create(buffer);
    const result = parser.parse();
    return result.tags.Orientation || 1;
  } catch (error) {
    return 1; // Default orientation if EXIF parsing fails
  }
}

async function loadImageWithHeicSupport(inputPath) {
  const ext = path.extname(inputPath).toLowerCase();

  // Check if HEIC/HEIF format
  if (ext === ".heic" || ext === ".heif") {
    console.log(`  Converting HEIC to JPEG...`);
    const inputBuffer = fs.readFileSync(inputPath);
    const outputBuffer = await heicConvert({
      buffer: inputBuffer,
      format: "JPEG",
      quality: 1.0,
    });
    // Load from converted buffer
    return await loadImage(outputBuffer);
  }

  // Load normally for other formats
  return await loadImage(inputPath);
}

async function processImageFile(
  inputPath,
  outputBmp,
  outputThumb,
  options,
  devicePalette = null,
) {
  console.log(`Processing: ${inputPath}`);

  // 1. Load image (with HEIC conversion if needed)
  const img = await loadImageWithHeicSupport(inputPath);
  let canvas = createCanvas(img.width, img.height);
  let ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  console.log(`  Original size: ${canvas.width}x${canvas.height}`);

  // 1.5. Apply EXIF orientation
  const orientation = getExifOrientation(inputPath);
  if (orientation > 1) {
    console.log(`  Applying EXIF orientation: ${orientation}`);
    canvas = applyExifOrientation(canvas, orientation, createCanvas);
    console.log(`  After EXIF correction: ${canvas.width}x${canvas.height}`);
  }

  // Save EXIF-corrected canvas for thumbnail generation (before rotation/processing)
  const originalCanvas = createCanvas(canvas.width, canvas.height);
  const originalCanvasCtx = originalCanvas.getContext("2d");
  originalCanvasCtx.drawImage(canvas, 0, 0);

  // 2. Check if portrait and rotate (skip rotation if rendering measured for debugging)
  const isPortrait = canvas.height > canvas.width;
  if (isPortrait && !options.renderMeasured) {
    console.log(`  Portrait detected, rotating 90° clockwise`);
    canvas = rotate90Clockwise(canvas, createCanvas);
    console.log(`  After rotation: ${canvas.width}x${canvas.height}`);
  } else if (isPortrait && options.renderMeasured) {
    console.log(`  Portrait detected, skipping rotation (debug mode)`);
  }

  // 3. Resize with cover (fill and crop)
  let targetWidth, targetHeight;
  if (options.renderMeasured && isPortrait) {
    targetWidth = DISPLAY_HEIGHT;
    targetHeight = DISPLAY_WIDTH; // 480x800
  } else {
    targetWidth = DISPLAY_WIDTH;
    targetHeight = DISPLAY_HEIGHT; // 800x480
  }

  if (canvas.width !== targetWidth || canvas.height !== targetHeight) {
    console.log(
      `  Resizing to ${targetWidth}x${targetHeight} (cover mode: scale and crop)`,
    );
    canvas = resizeImageCover(canvas, targetWidth, targetHeight, createCanvas);
  }

  // 4. Apply image processing
  ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);

  // Convert device palette to array format if provided
  let customPalette = null;
  if (devicePalette) {
    customPalette = [
      [devicePalette.black.r, devicePalette.black.g, devicePalette.black.b],
      [devicePalette.white.r, devicePalette.white.g, devicePalette.white.b],
      [devicePalette.yellow.r, devicePalette.yellow.g, devicePalette.yellow.b],
      [devicePalette.red.r, devicePalette.red.g, devicePalette.red.b],
      [0, 0, 0], // Reserved
      [devicePalette.blue.r, devicePalette.blue.g, devicePalette.blue.b],
      [devicePalette.green.r, devicePalette.green.g, devicePalette.green.b],
    ];
    console.log(`  Using calibrated color palette from device`);
  }

  const params = {
    exposure: options.exposure,
    saturation: options.saturation,
    toneMode: options.toneMode,
    contrast: options.contrast,
    strength: options.scurveStrength,
    shadowBoost: options.scurveShadow,
    highlightCompress: options.scurveHighlight,
    midpoint: options.scurveMidpoint,
    colorMethod: options.colorMethod,
    renderMeasured: options.renderMeasured,
    processingMode: options.processingMode,
    customPalette: customPalette,
  };

  if (params.processingMode === "stock") {
    console.log(
      `  Using stock Waveshare algorithm (no tone mapping, theoretical palette)`,
    );
  } else {
    console.log(`  Using enhanced algorithm`);
    console.log(
      `  Exposure: ${params.exposure}, Saturation: ${params.saturation}`,
    );
    if (params.toneMode === "scurve") {
      console.log(
        `  Tone mapping: S-Curve (strength=${params.strength}, shadow=${params.shadowBoost}, highlight=${params.highlightCompress})`,
      );
    } else {
      console.log(`  Tone mapping: Simple Contrast (${params.contrast})`);
    }
    console.log(`  Color method: ${params.colorMethod}`);
  }

  if (options.renderMeasured) {
    console.log(
      `  Rendering BMP with measured colors (darker output for preview)`,
    );
  } else {
    console.log(`  Rendering BMP with theoretical colors (standard output)`);
  }

  processImage(imageData, params);
  ctx.putImageData(imageData, 0, 0);

  // 5. Write BMP
  console.log(`  Writing BMP: ${outputBmp}`);
  writeBMP(imageData, outputBmp);

  // 6. Generate thumbnail if requested (from EXIF-corrected source, not processed image)
  if (options.generateThumbnail && outputThumb) {
    console.log(`  Generating thumbnail: ${outputThumb}`);

    // Use shared thumbnail generation function
    const thumbCanvas = generateThumbnail(
      originalCanvas,
      400,
      240,
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
  .option("--no-thumbnail", "Skip thumbnail generation")
  .option("--device-parameters", "Fetch processing parameters from device")
  .option(
    "--device-host <host>",
    "Device hostname or IP address (default: photoframe.local)",
    "photoframe.local",
  )
  .option(
    "--exposure <value>",
    "Exposure multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
    DEFAULT_PARAMS.exposure,
  )
  .option(
    "--saturation <value>",
    "Saturation multiplier (0.5-2.0, 1.0=normal)",
    parseFloat,
    DEFAULT_PARAMS.saturation,
  )
  .option(
    "--tone-mode <mode>",
    "Tone mapping mode: scurve or contrast",
    DEFAULT_PARAMS.toneMode,
  )
  .option(
    "--contrast <value>",
    "Contrast multiplier for simple mode (0.5-2.0, 1.0=normal)",
    parseFloat,
    DEFAULT_PARAMS.contrast,
  )
  .option(
    "--scurve-strength <value>",
    "S-curve overall strength (0.0-1.0)",
    parseFloat,
    DEFAULT_PARAMS.strength,
  )
  .option(
    "--scurve-shadow <value>",
    "S-curve shadow boost (0.0-1.0)",
    parseFloat,
    DEFAULT_PARAMS.shadowBoost,
  )
  .option(
    "--scurve-highlight <value>",
    "S-curve highlight compress (0.5-5.0)",
    parseFloat,
    DEFAULT_PARAMS.highlightCompress,
  )
  .option(
    "--scurve-midpoint <value>",
    "S-curve midpoint (0.3-0.7)",
    parseFloat,
    DEFAULT_PARAMS.midpoint,
  )
  .option(
    "--color-method <method>",
    "Color matching: rgb or lab",
    DEFAULT_PARAMS.colorMethod,
  )
  .option(
    "--render-measured",
    "Render BMP with measured palette colors (darker output for preview)",
  )
  .option(
    "--processing-mode <mode>",
    "Processing algorithm: enhanced (with tone mapping) or stock (Waveshare original)",
    DEFAULT_PARAMS.processingMode,
  )
  .action(async (input, options) => {
    try {
      const inputPath = path.resolve(input);
      if (!fs.existsSync(inputPath)) {
        console.error(`Error: Input path not found: ${inputPath}`);
        process.exit(1);
      }

      const outputDir = path.resolve(options.outputDir);
      if (!fs.existsSync(outputDir)) {
        fs.mkdirSync(outputDir, { recursive: true });
      }

      // Fetch device settings if --device-parameters is specified
      let deviceSettings = null;
      let devicePalette = null;
      if (options.deviceParameters) {
        try {
          deviceSettings = await fetchDeviceSettings(options.deviceHost);
          devicePalette = await fetchDevicePalette(options.deviceHost);
        } catch (error) {
          console.error(`Error: ${error.message}`);
          process.exit(1);
        }
      }

      // Use device settings if available, otherwise use CLI options
      // Note: renderMeasured is CLI-only and always comes from options
      const processOptions = deviceSettings
        ? {
            generateThumbnail: options.thumbnail,
            exposure: deviceSettings.exposure,
            saturation: deviceSettings.saturation,
            toneMode: deviceSettings.toneMode,
            contrast: deviceSettings.contrast,
            scurveStrength: deviceSettings.strength,
            scurveShadow: deviceSettings.shadowBoost,
            scurveHighlight: deviceSettings.highlightCompress,
            scurveMidpoint: deviceSettings.midpoint,
            colorMethod: deviceSettings.colorMethod,
            renderMeasured: options.renderMeasured || false,
            processingMode: deviceSettings.processingMode,
          }
        : {
            generateThumbnail: options.thumbnail,
            exposure: options.exposure,
            saturation: options.saturation,
            toneMode: options.toneMode,
            contrast: options.contrast,
            scurveStrength: options.scurveStrength,
            scurveShadow: options.scurveShadow,
            scurveHighlight: options.scurveHighlight,
            scurveMidpoint: options.scurveMidpoint,
            colorMethod: options.colorMethod,
            renderMeasured: options.renderMeasured || false,
            processingMode: options.processingMode,
          };

      // Automatically detect if input is file or directory
      const stats = fs.statSync(inputPath);
      if (stats.isDirectory()) {
        // Process folder structure (albums)
        await processFolderStructure(
          inputPath,
          outputDir,
          processOptions,
          deviceSettings,
          devicePalette,
        );
      } else {
        // Process single file
        const baseName = path.basename(input, path.extname(input));
        const suffix = options.suffix || "";
        const outputBmp = path.join(outputDir, `${baseName}${suffix}.bmp`);
        const outputThumb = options.thumbnail
          ? path.join(outputDir, `${baseName}${suffix}.jpg`)
          : null;

        await processImageFile(
          inputPath,
          outputBmp,
          outputThumb,
          processOptions,
          devicePalette,
        );
      }
    } catch (error) {
      console.error(`Error processing: ${error.message}`);
      console.error(error.stack);
      process.exit(1);
    }
  });

program.parse();
