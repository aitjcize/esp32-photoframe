/**
 * CLI utility functions for image processing
 * Handles file loading, HEIC conversion, and EXIF orientation
 */

import fs from "fs";
import { loadImage, createCanvas } from "canvas";
import exifParser from "exif-parser";
import heicConvert from "heic-convert";
import {
  processImage,
  applyExifOrientation,
  rotateImage,
  SPECTRA6,
} from "@aitjcize/epaper-image-convert";

/**
 * Load image with HEIC support
 * @param {string} imagePath - Path to image file
 * @returns {Promise<Image>} Loaded image
 */
async function loadImageWithHeicSupport(imagePath) {
  const ext = imagePath.toLowerCase();
  if (ext.endsWith(".heic") || ext.endsWith(".heif")) {
    const inputBuffer = fs.readFileSync(imagePath);
    const outputBuffer = await heicConvert({
      buffer: inputBuffer,
      format: "JPEG",
      quality: 1,
    });
    return await loadImage(outputBuffer);
  }
  return await loadImage(imagePath);
}

/**
 * Get EXIF orientation from image file
 * @param {string} imagePath - Path to image file
 * @returns {number} EXIF orientation value (1-8)
 */
function getExifOrientation(imagePath) {
  try {
    const buffer = fs.readFileSync(imagePath);
    const parser = exifParser.create(buffer);
    const result = parser.parse();
    return result.tags.Orientation || 1;
  } catch (error) {
    return 1;
  }
}

/**
 * Process image pipeline: load, apply EXIF, process, return canvas
 * @param {string} imagePath - Path to image file
 * @param {Object} processingParams - Processing parameters
 * @param {number} displayWidth - Display width in pixels
 * @param {number} displayHeight - Display height in pixels
 * @param {Object} devicePalette - Optional device-specific palette
 * @param {Object} options - Processing options:
 *   - verbose {boolean} - Enable verbose logging
 *   - skipDithering {boolean} - Skip dithering step
 *   - autoOrient {boolean} - Auto-rotate image to match target orientation
 *   - orientation {string} - Display orientation: "landscape" or "portrait"
 *   - scaleMode {string} - Scale mode: "cover" or "fit" (default: "cover")
 *   - backgroundColor {string} - Palette color name for fit mode background (default: "white")
 * @returns {Promise<Object>} { canvas, originalCanvas, thumbnail }
 */
export async function processImagePipeline(
  imagePath,
  processingParams,
  displayWidth,
  displayHeight,
  devicePalette = null,
  options = {},
) {
  const {
    verbose = false,
    skipDithering = false,
    autoOrient = false,
    orientation = "landscape",
    scaleMode = "cover",
    backgroundColor = "white",
  } = options;

  // Load image (with HEIC conversion if needed)
  const img = await loadImageWithHeicSupport(imagePath);
  let canvas = createCanvas(img.width, img.height);
  const ctx = canvas.getContext("2d");
  ctx.drawImage(img, 0, 0);

  // Apply EXIF orientation
  const exifOrientation = getExifOrientation(imagePath);
  if (exifOrientation > 1) {
    if (verbose) {
      console.log(`  Applying EXIF orientation: ${exifOrientation}`);
    }
    canvas = applyExifOrientation(canvas, exifOrientation, createCanvas);
    if (verbose) {
      console.log(`  After EXIF correction: ${canvas.width}x${canvas.height}`);
    }
  }

  // Auto-orient: rotate to match target orientation if they differ
  if (autoOrient) {
    const isSourcePortrait = canvas.height > canvas.width;
    const isTargetPortrait = displayHeight > displayWidth;
    if (isSourcePortrait !== isTargetPortrait) {
      canvas = rotateImage(canvas, 90, createCanvas);
      if (verbose) {
        console.log(
          `  Auto-oriented: rotated 90° to match ${isTargetPortrait ? "portrait" : "landscape"} target`,
        );
      }
    }
  }

  // Build palette object for the library
  // The library expects { theoretical, perceived } format
  // devicePalette from the device is the "perceived" palette
  let palette;
  if (devicePalette) {
    // Use SPECTRA6 theoretical with device-provided perceived palette
    palette = {
      theoretical: SPECTRA6.theoretical,
      perceived: devicePalette,
    };
  } else {
    // Use default SPECTRA6 palette
    palette = SPECTRA6;
  }

  // Call shared processImage pipeline (handles rotation, resize, preprocessing, dithering)
  return processImage(canvas, {
    displayWidth,
    displayHeight,
    palette,
    params: processingParams,
    orientation,
    scaleMode,
    backgroundColor,
    verbose,
    createCanvas,
    skipDithering,
  });
}
