#!/usr/bin/env node

import { Command } from "commander";
import fs from "fs";
import path from "path";
import os from "os";
import http from "http";
import url from "url";
import { fileURLToPath } from "url";
import { loadImage, createCanvas } from "canvas";
import exifParser from "exif-parser";
import heicConvert from "heic-convert";
import FormData from "form-data";
import fetch from "node-fetch";
import {
  processImage,
  generateThumbnail,
  createPNG,
} from "./image-processor.js";
import { processImagePipeline } from "./utils.js";
import { createImageServer } from "./server.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Display dimensions constants
const DISPLAY_WIDTH_LANDSCAPE = 800;
const DISPLAY_HEIGHT_LANDSCAPE = 480;
const DISPLAY_WIDTH_PORTRAIT = 480;
const DISPLAY_HEIGHT_PORTRAIT = 800;

// Thumbnail dimensions (half of display resolution)
const THUMBNAIL_WIDTH = 400;
const THUMBNAIL_HEIGHT = 240;

// Legacy constants for backwards compatibility
const DISPLAY_WIDTH = DISPLAY_WIDTH_LANDSCAPE;
const DISPLAY_HEIGHT = DISPLAY_HEIGHT_LANDSCAPE;

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
              console.log(`✓ Upload successful: ${response.filename}`);
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
  deviceSettings,
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

  // Build processing parameters from options
  const processingParams = {
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
  };

  // Use shared processing pipeline with verbose logging
  const { canvas, originalCanvas } = await processImagePipeline(
    inputPath,
    processingParams,
    devicePalette,
    { verbose: true },
  );

  const ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);

  // 5. Write output file (BMP or PNG)
  if (options.outputPng) {
    const outputPng = outputBmp.replace(/\.bmp$/, ".png");
    console.log(`  Writing PNG: ${outputPng}`);
    const pngBuffer = await createPNG(canvas);
    fs.writeFileSync(outputPng, pngBuffer);
  } else {
    console.log(`  Writing BMP: ${outputBmp}`);
    writeBMP(imageData, outputBmp);
  }

  // 6. Generate thumbnail if requested (from EXIF-corrected source, not processed image)
  if (options.generateThumbnail && outputThumb) {
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

        // Fetch device settings if --device-parameters is specified
        let deviceSettings = null;
        let devicePalette = null;
        if (options.deviceParameters) {
          try {
            console.log(`Fetching device parameters from ${options.host}...`);
            deviceSettings = await fetchDeviceSettings(options.host);
            devicePalette = await fetchDevicePalette(options.host);
            console.log(`Device parameters fetched successfully`);
          } catch (error) {
            console.error(`Error fetching device parameters: ${error.message}`);
            console.error(`Falling back to default parameters`);
          }
        }

        // Start HTTP server and keep running
        await createImageServer(
          inputPath,
          port,
          options.serveFormat,
          devicePalette,
          deviceSettings,
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

      // Use tmpdir when uploading or displaying directly, otherwise use specified output directory
      let outputDir;
      let useTmpDir = false;
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

      // Use device settings if available, otherwise use CLI options
      // Note: renderMeasured is CLI-only and always comes from options
      // Always output PNG and generate thumbnails (matching webapp behavior)
      const processOptions = deviceSettings
        ? {
            generateThumbnail: true,
            outputPng: true,
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
            generateThumbnail: true,
            outputPng: true,
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

      // Process based on input type
      if (isDirectory) {
        // Process folder structure (albums)
        await processFolderStructure(
          inputPath,
          outputDir,
          processOptions,
          deviceSettings,
          devicePalette,
          options.upload ? options.host : null,
        );
      } else {
        // Process single file
        const baseName = path.basename(input, path.extname(input));
        const suffix = options.suffix || "";
        const outputPng = path.join(outputDir, `${baseName}${suffix}.png`);
        const outputThumb = path.join(outputDir, `${baseName}${suffix}.jpg`);

        await processImageFile(
          inputPath,
          outputPng,
          outputThumb,
          processOptions,
          devicePalette,
        );

        // Upload or display directly on device
        if (options.upload || options.direct) {
          if (!fs.existsSync(outputPng)) {
            console.error(`Error: PNG file not found: ${outputPng}`);
            process.exit(1);
          }
          if (!fs.existsSync(outputThumb)) {
            console.error(`Error: Thumbnail file not found: ${outputThumb}`);
            process.exit(1);
          }

          try {
            if (options.direct) {
              await displayDirectly(options.host, outputPng, outputThumb);
            } else {
              await uploadToDevice(options.host, outputPng, outputThumb, null);
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
