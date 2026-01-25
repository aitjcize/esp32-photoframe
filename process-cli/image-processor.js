// Image processing functions for S-curve tone mapping and dithering
// This file is shared between the webapp and CLI

// Display dimensions constants
const DISPLAY_WIDTH_LANDSCAPE = 800;
const DISPLAY_HEIGHT_LANDSCAPE = 480;
const DISPLAY_WIDTH_PORTRAIT = 480;
const DISPLAY_HEIGHT_PORTRAIT = 800;

// Thumbnail dimensions (half of display resolution)
const THUMBNAIL_WIDTH = 400;
const THUMBNAIL_HEIGHT = 240;

// Helper function to get canvas context with image smoothing disabled
function getCanvasContext(canvas, contextType = "2d") {
  const ctx = canvas.getContext(contextType);
  if (ctx && contextType === "2d") {
    ctx.imageSmoothingEnabled = false;
    // Vendor prefixes for older browsers/environments
    if (ctx.mozImageSmoothingEnabled !== undefined) {
      ctx.mozImageSmoothingEnabled = false;
    }
    if (ctx.webkitImageSmoothingEnabled !== undefined) {
      ctx.webkitImageSmoothingEnabled = false;
    }
    if (ctx.msImageSmoothingEnabled !== undefined) {
      ctx.msImageSmoothingEnabled = false;
    }
  }
  return ctx;
}

// Measured palette - actual displayed colors from e-paper
const PALETTE_MEASURED = [
  [2, 2, 2], // Black
  [190, 190, 190], // White
  [205, 202, 0], // Yellow
  [135, 19, 0], // Red
  [0, 0, 0], // Reserved (not used)
  [5, 64, 158], // Blue
  [39, 102, 60], // Green
];

// Theoretical palette - for BMP output
const PALETTE_THEORETICAL = [
  [0, 0, 0], // Black
  [255, 255, 255], // White
  [255, 255, 0], // Yellow
  [255, 0, 0], // Red
  [0, 0, 0], // Reserved
  [0, 0, 255], // Blue
  [0, 255, 0], // Green
];

function applyExposure(imageData, exposure) {
  if (exposure === 1.0) return;

  const data = imageData.data;

  for (let i = 0; i < data.length; i += 4) {
    data[i] = Math.min(255, Math.round(data[i] * exposure));
    data[i + 1] = Math.min(255, Math.round(data[i + 1] * exposure));
    data[i + 2] = Math.min(255, Math.round(data[i + 2] * exposure));
  }
}

function applyContrast(imageData, contrast) {
  if (contrast === 1.0) return;

  const data = imageData.data;
  const factor = contrast;

  for (let i = 0; i < data.length; i += 4) {
    // Apply contrast adjustment around midpoint (128)
    data[i] = Math.max(
      0,
      Math.min(255, Math.round((data[i] - 128) * factor + 128)),
    );
    data[i + 1] = Math.max(
      0,
      Math.min(255, Math.round((data[i + 1] - 128) * factor + 128)),
    );
    data[i + 2] = Math.max(
      0,
      Math.min(255, Math.round((data[i + 2] - 128) * factor + 128)),
    );
  }
}

function applySaturation(imageData, saturation) {
  if (saturation === 1.0) return;

  const data = imageData.data;

  for (let i = 0; i < data.length; i += 4) {
    const r = data[i];
    const g = data[i + 1];
    const b = data[i + 2];

    // Convert RGB to HSL
    const max = Math.max(r, g, b) / 255;
    const min = Math.min(r, g, b) / 255;
    const l = (max + min) / 2;

    if (max === min) {
      // Grayscale - no saturation change needed
      continue;
    }

    const d = max - min;
    const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);

    let h;
    if (max === r / 255) {
      h = ((g / 255 - b / 255) / d + (g < b ? 6 : 0)) / 6;
    } else if (max === g / 255) {
      h = ((b / 255 - r / 255) / d + 2) / 6;
    } else {
      h = ((r / 255 - g / 255) / d + 4) / 6;
    }

    // Adjust saturation
    const newS = Math.max(0, Math.min(1, s * saturation));

    // Convert back to RGB
    const c = (1 - Math.abs(2 * l - 1)) * newS;
    const x = c * (1 - Math.abs(((h * 6) % 2) - 1));
    const m = l - c / 2;

    let rPrime, gPrime, bPrime;
    const hSector = Math.floor(h * 6);

    if (hSector === 0) {
      [rPrime, gPrime, bPrime] = [c, x, 0];
    } else if (hSector === 1) {
      [rPrime, gPrime, bPrime] = [x, c, 0];
    } else if (hSector === 2) {
      [rPrime, gPrime, bPrime] = [0, c, x];
    } else if (hSector === 3) {
      [rPrime, gPrime, bPrime] = [0, x, c];
    } else if (hSector === 4) {
      [rPrime, gPrime, bPrime] = [x, 0, c];
    } else {
      [rPrime, gPrime, bPrime] = [c, 0, x];
    }

    data[i] = Math.round((rPrime + m) * 255);
    data[i + 1] = Math.round((gPrime + m) * 255);
    data[i + 2] = Math.round((bPrime + m) * 255);
  }
}

function applyScurveTonemap(
  imageData,
  strength,
  shadowBoost,
  highlightCompress,
  midpoint,
) {
  if (strength === 0) return;

  const data = imageData.data;

  for (let i = 0; i < data.length; i += 4) {
    for (let c = 0; c < 3; c++) {
      const normalized = data[i + c] / 255.0;
      let result;

      if (normalized <= midpoint) {
        // Shadows: brighten
        const shadowVal = normalized / midpoint;
        result = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
      } else {
        // Highlights: compress
        const highlightVal = (normalized - midpoint) / (1.0 - midpoint);
        result =
          midpoint +
          Math.pow(highlightVal, 1.0 + strength * highlightCompress) *
            (1.0 - midpoint);
      }

      data[i + c] = Math.round(Math.max(0, Math.min(1, result)) * 255);
    }
  }
}

// LAB color space conversion functions
function rgbToXyz(r, g, b) {
  r = r / 255;
  g = g / 255;
  b = b / 255;

  r = r > 0.04045 ? Math.pow((r + 0.055) / 1.055, 2.4) : r / 12.92;
  g = g > 0.04045 ? Math.pow((g + 0.055) / 1.055, 2.4) : g / 12.92;
  b = b > 0.04045 ? Math.pow((b + 0.055) / 1.055, 2.4) : b / 12.92;

  const x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375;
  const y = r * 0.2126729 + g * 0.7151522 + b * 0.072175;
  const z = r * 0.0193339 + g * 0.119192 + b * 0.9503041;

  return [x * 100, y * 100, z * 100];
}

function xyzToLab(x, y, z) {
  x = x / 95.047;
  y = y / 100.0;
  z = z / 108.883;

  x = x > 0.008856 ? Math.pow(x, 1 / 3) : 7.787 * x + 16 / 116;
  y = y > 0.008856 ? Math.pow(y, 1 / 3) : 7.787 * y + 16 / 116;
  z = z > 0.008856 ? Math.pow(z, 1 / 3) : 7.787 * z + 16 / 116;

  const L = 116 * y - 16;
  const a = 500 * (x - y);
  const b = 200 * (y - z);

  return [L, a, b];
}

function rgbToLab(r, g, b) {
  const [x, y, z] = rgbToXyz(r, g, b);
  return xyzToLab(x, y, z);
}

function deltaE(lab1, lab2) {
  const dL = lab1[0] - lab2[0];
  const da = lab1[1] - lab2[1];
  const db = lab1[2] - lab2[2];
  return Math.sqrt(dL * dL + da * da + db * db);
}

// Pre-compute LAB values for palette (done once)
const PALETTE_LAB = PALETTE_MEASURED.map(([r, g, b]) => rgbToLab(r, g, b));

function findClosestColorRGB(r, g, b, palette = PALETTE_MEASURED) {
  let minDist = Infinity;
  let closest = 1; // Default to white

  for (let i = 0; i < palette.length; i++) {
    if (i === 4) continue; // Skip reserved color

    const [pr, pg, pb] = palette[i];
    const dr = r - pr;
    const dg = g - pg;
    const db = b - pb;

    // Simple Euclidean distance in RGB space
    const dist = dr * dr + dg * dg + db * db;

    if (dist < minDist) {
      minDist = dist;
      closest = i;
    }
  }

  return closest;
}

function findClosestColorLAB(r, g, b) {
  let minDist = Infinity;
  let closest = 1; // Default to white

  // Convert input color to LAB
  const inputLab = rgbToLab(r, g, b);

  for (let i = 0; i < PALETTE_MEASURED.length; i++) {
    if (i === 4) continue; // Skip reserved color

    // Calculate perceptual distance in LAB space
    const dist = deltaE(inputLab, PALETTE_LAB[i]);

    if (dist < minDist) {
      minDist = dist;
      closest = i;
    }
  }

  return closest;
}

// Main color matching function - delegates based on method
function findClosestColor(r, g, b, method = "rgb", palette = PALETTE_MEASURED) {
  return method === "lab"
    ? findClosestColorLAB(r, g, b)
    : findClosestColorRGB(r, g, b, palette);
}

// Error diffusion dithering with configurable algorithm
function applyErrorDiffusionDither(
  imageData,
  method = "rgb",
  outputPalette = PALETTE_MEASURED,
  ditherPalette = PALETTE_MEASURED,
  algorithm = "floyd-steinberg",
) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;

  // Create error buffer
  const errors = new Array(width * height * 3).fill(0);

  // Define error diffusion matrices for different algorithms
  // Format: [dx, dy, weight]
  const diffusionMatrices = {
    "floyd-steinberg": [
      [1, 0, 7 / 16],
      [-1, 1, 3 / 16],
      [0, 1, 5 / 16],
      [1, 1, 1 / 16],
    ],
    stucki: [
      [1, 0, 8 / 42],
      [2, 0, 4 / 42],
      [-2, 1, 2 / 42],
      [-1, 1, 4 / 42],
      [0, 1, 8 / 42],
      [1, 1, 4 / 42],
      [2, 1, 2 / 42],
      [-2, 2, 1 / 42],
      [-1, 2, 2 / 42],
      [0, 2, 4 / 42],
      [1, 2, 2 / 42],
      [2, 2, 1 / 42],
    ],
    burkes: [
      [1, 0, 8 / 32],
      [2, 0, 4 / 32],
      [-2, 1, 2 / 32],
      [-1, 1, 4 / 32],
      [0, 1, 8 / 32],
      [1, 1, 4 / 32],
      [2, 1, 2 / 32],
    ],
    sierra: [
      [1, 0, 5 / 32],
      [2, 0, 3 / 32],
      [-2, 1, 2 / 32],
      [-1, 1, 4 / 32],
      [0, 1, 5 / 32],
      [1, 1, 4 / 32],
      [2, 1, 2 / 32],
      [-1, 2, 2 / 32],
      [0, 2, 3 / 32],
      [1, 2, 2 / 32],
    ],
  };

  const diffusionMatrix =
    diffusionMatrices[algorithm] || diffusionMatrices["floyd-steinberg"];

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 4;
      const errIdx = (y * width + x) * 3;

      // Get old pixel value with accumulated error
      let oldR = data[idx] + errors[errIdx];
      let oldG = data[idx + 1] + errors[errIdx + 1];
      let oldB = data[idx + 2] + errors[errIdx + 2];

      // Clamp values
      oldR = Math.max(0, Math.min(255, oldR));
      oldG = Math.max(0, Math.min(255, oldG));
      oldB = Math.max(0, Math.min(255, oldB));

      // Find closest color using dither palette
      const colorIdx = findClosestColor(
        oldR,
        oldG,
        oldB,
        method,
        ditherPalette,
      );
      const [newR, newG, newB] = outputPalette[colorIdx];

      // Set new pixel color (using output palette)
      data[idx] = newR;
      data[idx + 1] = newG;
      data[idx + 2] = newB;

      // Calculate error using dither palette (for error diffusion)
      const [ditherR, ditherG, ditherB] = ditherPalette[colorIdx];
      const errR = oldR - ditherR;
      const errG = oldG - ditherG;
      const errB = oldB - ditherB;

      // Distribute error to neighboring pixels using selected algorithm
      for (const [dx, dy, weight] of diffusionMatrix) {
        const nx = x + dx;
        const ny = y + dy;

        if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
          const nextIdx = (ny * width + nx) * 3;
          errors[nextIdx] += errR * weight;
          errors[nextIdx + 1] += errG * weight;
          errors[nextIdx + 2] += errB * weight;
        }
      }
    }
  }
}

function preprocessImage(imageData, params) {
  // Processing mode: 'stock' (Waveshare original) or 'enhanced' (our algorithm)
  const mode = params.processingMode || "enhanced";
  const toneMode = params.toneMode || "scurve"; // 'contrast' or 'scurve'

  if (mode === "stock") {
    // Stock Waveshare algorithm: no tone mapping
    // Dithering will be applied later in processImage if needed
    return;
  }

  // Enhanced algorithm with tone mapping

  // 1. Apply exposure
  if (params.exposure && params.exposure !== 1.0) {
    applyExposure(imageData, params.exposure);
  }

  // 2. Apply saturation
  if (params.saturation !== 1.0) {
    applySaturation(imageData, params.saturation);
  }

  // 3. Apply tone mapping (contrast or S-curve)
  if (toneMode === "contrast") {
    if (params.contrast && params.contrast !== 1.0) {
      applyContrast(imageData, params.contrast);
    }
  } else {
    // S-curve tone mapping
    applyScurveTonemap(
      imageData,
      params.strength,
      params.shadowBoost,
      params.highlightCompress,
      params.midpoint,
    );
  }
}

/**
 * Rotate canvas 90 degrees clockwise
 * @param {Canvas} canvas - Source canvas (browser Canvas or node-canvas)
 * @param {Function} createCanvas - Canvas creation function (for Node.js compatibility)
 * @returns {Canvas} Rotated canvas
 */
function rotate90Clockwise(canvas, createCanvas = null) {
  let rotatedCanvas;
  if (createCanvas) {
    rotatedCanvas = createCanvas(canvas.height, canvas.width);
  } else {
    rotatedCanvas = document.createElement("canvas");
    rotatedCanvas.width = canvas.height;
    rotatedCanvas.height = canvas.width;
  }

  const ctx = getCanvasContext(rotatedCanvas);
  ctx.translate(canvas.height, 0);
  ctx.rotate(Math.PI / 2);
  ctx.drawImage(canvas, 0, 0);

  return rotatedCanvas;
}

/**
 * Apply EXIF orientation transformation to canvas
 * @param {Canvas} canvas - Source canvas (browser Canvas or node-canvas)
 * @param {number} orientation - EXIF orientation value (1-8)
 * @param {Function} createCanvas - Canvas creation function (for Node.js compatibility)
 * @returns {Canvas} Transformed canvas
 */
function applyExifOrientation(canvas, orientation, createCanvas = null) {
  if (orientation === 1) return canvas;

  const { width, height } = canvas;
  let newCanvas, ctx;

  // Set canvas dimensions based on orientation
  if (orientation >= 5 && orientation <= 8) {
    // Rotations that swap width/height
    if (createCanvas) {
      newCanvas = createCanvas(height, width);
    } else {
      newCanvas = document.createElement("canvas");
      newCanvas.width = height;
      newCanvas.height = width;
    }
  } else {
    if (createCanvas) {
      newCanvas = createCanvas(width, height);
    } else {
      newCanvas = document.createElement("canvas");
      newCanvas.width = width;
      newCanvas.height = height;
    }
  }

  ctx = getCanvasContext(newCanvas);

  // Apply transformations based on EXIF orientation
  switch (orientation) {
    case 2:
      ctx.transform(-1, 0, 0, 1, width, 0);
      break;
    case 3:
      ctx.transform(-1, 0, 0, -1, width, height);
      break;
    case 4:
      ctx.transform(1, 0, 0, -1, 0, height);
      break;
    case 5:
      ctx.transform(0, 1, 1, 0, 0, 0);
      break;
    case 6:
      ctx.transform(0, 1, -1, 0, height, 0);
      break;
    case 7:
      ctx.transform(0, -1, -1, 0, height, width);
      break;
    case 8:
      ctx.transform(0, -1, 1, 0, 0, width);
      break;
  }

  ctx.drawImage(canvas, 0, 0);
  return newCanvas;
}

/**
 * Resize image with cover mode (scale and crop to fill)
 * @param {Canvas} sourceCanvas - Source canvas (browser Canvas or node-canvas)
 * @param {number} targetWidth - Target width
 * @param {number} targetHeight - Target height
 * @param {Function} createCanvas - Canvas creation function (for Node.js compatibility)
 * @returns {Canvas} Resized canvas
 */
function resizeImageCover(
  sourceCanvas,
  targetWidth,
  targetHeight,
  createCanvas = null,
) {
  const srcWidth = sourceCanvas.width;
  const srcHeight = sourceCanvas.height;

  // Calculate scale to cover (larger of the two ratios)
  const scaleX = targetWidth / srcWidth;
  const scaleY = targetHeight / srcHeight;
  const scale = Math.max(scaleX, scaleY);

  const scaledWidth = Math.round(srcWidth * scale);
  const scaledHeight = Math.round(srcHeight * scale);

  // Create temporary canvas for scaling
  let tempCanvas;
  if (createCanvas) {
    tempCanvas = createCanvas(scaledWidth, scaledHeight);
  } else {
    tempCanvas = document.createElement("canvas");
    tempCanvas.width = scaledWidth;
    tempCanvas.height = scaledHeight;
  }
  const tempCtx = getCanvasContext(tempCanvas);
  tempCtx.drawImage(sourceCanvas, 0, 0, scaledWidth, scaledHeight);

  // Crop to target size (center crop)
  const cropX = Math.round((scaledWidth - targetWidth) / 2);
  const cropY = Math.round((scaledHeight - targetHeight) / 2);

  let outputCanvas;
  if (createCanvas) {
    outputCanvas = createCanvas(targetWidth, targetHeight);
  } else {
    outputCanvas = document.createElement("canvas");
    outputCanvas.width = targetWidth;
    outputCanvas.height = targetHeight;
  }
  const outputCtx = getCanvasContext(outputCanvas);
  outputCtx.drawImage(
    tempCanvas,
    cropX,
    cropY,
    targetWidth,
    targetHeight,
    0,
    0,
    targetWidth,
    targetHeight,
  );

  return outputCanvas;
}

/**
 * Generate thumbnail from canvas with proper orientation handling
 * @param {Canvas} sourceCanvas - Source canvas (browser Canvas or node-canvas)
 * @param {number} targetWidth - Target width for landscape orientation
 * @param {number} targetHeight - Target height for landscape orientation
 * @param {Function} createCanvas - Canvas creation function (for Node.js compatibility)
 * @returns {Canvas} Thumbnail canvas
 */
function generateThumbnail(
  sourceCanvas,
  targetWidth = THUMBNAIL_WIDTH,
  targetHeight = THUMBNAIL_HEIGHT,
  createCanvas = null,
) {
  const srcWidth = sourceCanvas.width;
  const srcHeight = sourceCanvas.height;

  // Maintain source orientation - swap dimensions for portrait images
  const isPortrait = srcHeight > srcWidth;
  const thumbWidth = isPortrait ? targetHeight : targetWidth;
  const thumbHeight = isPortrait ? targetWidth : targetHeight;

  // Create thumbnail canvas
  let thumbCanvas;
  if (createCanvas) {
    // Node.js environment
    thumbCanvas = createCanvas(thumbWidth, thumbHeight);
  } else {
    // Browser environment
    thumbCanvas = document.createElement("canvas");
    thumbCanvas.width = thumbWidth;
    thumbCanvas.height = thumbHeight;
  }

  const thumbCtx = getCanvasContext(thumbCanvas);

  // Scale to cover thumbnail size (crop to fill)
  const scaleX = thumbWidth / srcWidth;
  const scaleY = thumbHeight / srcHeight;
  const scale = Math.max(scaleX, scaleY);

  const scaledWidth = Math.round(srcWidth * scale);
  const scaledHeight = Math.round(srcHeight * scale);
  const cropX = Math.round((scaledWidth - thumbWidth) / 2);
  const cropY = Math.round((scaledHeight - thumbHeight) / 2);

  // Draw scaled and cropped image
  thumbCtx.drawImage(
    sourceCanvas,
    cropX / scale,
    cropY / scale,
    thumbWidth / scale,
    thumbHeight / scale,
    0,
    0,
    thumbWidth,
    thumbHeight,
  );

  return thumbCanvas;
}

/**
 * Convert canvas to PNG (24-bit RGB)
 * Note: Indexed PNG encoding is not reliably supported in JavaScript libraries
 * @param {HTMLCanvasElement} canvas - Canvas with dithered image data
 * @returns {Promise<Blob>|Buffer} PNG blob (browser) or Buffer (Node.js)
 */
async function createPNG(canvas) {
  const width = canvas.width;
  const height = canvas.height;

  // Use native PNG encoding (24-bit RGB)
  // Note: Indexed PNG encoding is not reliably supported in JavaScript
  // 24-bit PNG is still 4-7x smaller than BMP and uploads much faster
  if (typeof Buffer !== "undefined" && typeof window === "undefined") {
    // Node.js environment
    const buffer = canvas.toBuffer("image/png");
    return buffer;
  } else {
    // Browser environment
    return new Promise((resolve, reject) => {
      canvas.toBlob((blob) => {
        if (blob) {
          resolve(blob);
        } else {
          reject(new Error("Failed to create PNG blob"));
        }
      }, "image/png");
    });
  }
}

/**
 * Complete image processing pipeline from source to device-ready output
 * Handles: portrait rotation, resizing, tone mapping, dithering
 *
 * @param {Canvas|ImageData} source - Source canvas (should already have EXIF orientation applied)
 * @param {Object} processingParams - Processing parameters
 * @param {Object} devicePalette - Optional device-specific palette
 * @param {Object} options - Additional options (verbose, targetWidth, targetHeight, createCanvas, skipRotation, skipDithering)
 * @returns {Object} { canvas: processed canvas, originalCanvas: EXIF-corrected source }
 */
function processImage(
  source,
  processingParams,
  devicePalette = null,
  options = {},
) {
  const {
    verbose = false,
    targetWidth = DISPLAY_WIDTH_LANDSCAPE,
    targetHeight = DISPLAY_HEIGHT_LANDSCAPE,
    createCanvas = null,
    skipRotation = false,
    skipDithering = false,
  } = options;

  // Handle both canvas and ImageData inputs
  // Create a new canvas to avoid mutating the source
  // Note: In Node.js, ImageData is not a global, so we use duck typing
  const isImageData = source.data && source.width && source.height;
  let canvas;
  if (createCanvas) {
    canvas = createCanvas(source.width, source.height);
  } else {
    canvas = document.createElement("canvas");
    canvas.width = source.width;
    canvas.height = source.height;
  }
  const ctx = getCanvasContext(canvas);

  // Copy source to canvas
  if (isImageData) {
    ctx.putImageData(source, 0, 0);
  } else {
    ctx.drawImage(source, 0, 0);
  }

  if (verbose) {
    console.log(`  Original size: ${canvas.width}x${canvas.height}`);
  }

  // Save EXIF-corrected canvas for thumbnail generation (before rotation/processing)
  let originalCanvas;
  if (createCanvas) {
    originalCanvas = createCanvas(canvas.width, canvas.height);
  } else {
    originalCanvas = document.createElement("canvas");
    originalCanvas.width = canvas.width;
    originalCanvas.height = canvas.height;
  }
  const originalCanvasCtx = getCanvasContext(originalCanvas);
  originalCanvasCtx.drawImage(canvas, 0, 0);

  // Check if portrait and rotate to landscape (unless skipRotation is true)
  const isPortrait = canvas.height > canvas.width;
  if (isPortrait && !skipRotation) {
    if (verbose) {
      console.log(`  Portrait detected, rotating 90° clockwise`);
    }
    canvas = rotate90Clockwise(canvas, createCanvas);
    if (verbose) {
      console.log(`  After rotation: ${canvas.width}x${canvas.height}`);
    }
  } else if (isPortrait && skipRotation && verbose) {
    console.log(
      `  Portrait detected, skipping rotation (browser preview mode)`,
    );
  }

  // Resize to display dimensions
  // If portrait and skipRotation, use portrait dimensions; otherwise landscape
  let finalWidth, finalHeight;
  if (isPortrait && skipRotation) {
    finalWidth = targetWidth; // Portrait width
    finalHeight = targetHeight; // Portrait height
  } else {
    finalWidth = targetWidth; // Landscape width
    finalHeight = targetHeight; // Landscape height
  }

  if (canvas.width !== finalWidth || canvas.height !== finalHeight) {
    if (verbose) {
      console.log(
        `  Resizing to ${finalWidth}x${finalHeight} (cover mode: scale and crop)`,
      );
    }
    canvas = resizeImageCover(canvas, finalWidth, finalHeight, createCanvas);
  }

  // Apply image processing (tone mapping, dithering, palette conversion)
  const imageData = getCanvasContext(canvas).getImageData(
    0,
    0,
    canvas.width,
    canvas.height,
  );

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
    if (verbose) {
      console.log(`  Using calibrated color palette from device`);
    }
  }

  const params = {
    ...processingParams,
    customPalette: customPalette,
    skipDithering: skipDithering,
  };

  if (verbose) {
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

    if (params.renderMeasured) {
      console.log(
        `  Rendering BMP with measured colors (darker output for preview)`,
      );
    } else {
      console.log(`  Rendering BMP with theoretical colors (standard output)`);
    }
  }

  // Apply tone mapping (exposure, saturation, contrast/S-curve)
  preprocessImage(imageData, params);

  // Apply dithering if not skipped
  if (!skipDithering) {
    const mode = params.processingMode || "enhanced";
    const ditherPalette = customPalette || PALETTE_MEASURED;
    const ditherAlgorithm = params.ditherAlgorithm || "floyd-steinberg";

    if (mode === "stock") {
      // Stock Waveshare algorithm: theoretical palette for dithering
      const outputPalette = params.renderMeasured
        ? ditherPalette
        : PALETTE_THEORETICAL;
      applyErrorDiffusionDither(
        imageData,
        "rgb",
        outputPalette,
        PALETTE_THEORETICAL,
        ditherAlgorithm,
      );
    } else {
      // Enhanced algorithm: use color method and measured palette
      const outputPalette = params.renderMeasured
        ? ditherPalette
        : PALETTE_THEORETICAL;
      applyErrorDiffusionDither(
        imageData,
        params.colorMethod,
        outputPalette,
        ditherPalette,
        ditherAlgorithm,
      );
    }
  }

  getCanvasContext(canvas).putImageData(imageData, 0, 0);

  return { canvas, originalCanvas };
}

// Export for Node.js ES modules
export {
  processImage,
  applyExifOrientation,
  resizeImageCover,
  generateThumbnail,
  createPNG,
  getCanvasContext,
};
