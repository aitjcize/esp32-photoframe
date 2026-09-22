// Framing math shared by the crop editor, the live preview and the upload.
//
// Vocabulary
//   frame   the panel in its logical orientation (e.g. 1200x1600 portrait, or
//           1600x1200 when the device is set to landscape)
//   image   the source photo after rotation / mirroring ("oriented")
//   rect    the region of the image that fills the frame. Same aspect ratio as
//           the frame. It may extend past the image edges: whatever it does not
//           cover is painted with the background colour, which is how a
//           letterbox ("fit") is expressed.
//
// Rects are kept in *normalized* image coordinates (fractions of the image
// width / height) whenever they cross a component boundary, so the same rect
// applies to the full-resolution source used for upload and to the downscaled
// copies used for previews. Pixel rects are used inside the editor.

import { rotateImage } from "@aitjcize/epaper-image-convert";

/** Size of an image after a rotation by a multiple of 90 degrees. */
export function orientedSize(width, height, rotation) {
  const r = normalizeRotation(rotation);
  return r === 90 || r === 270 ? { width: height, height: width } : { width, height };
}

export function normalizeRotation(rotation) {
  return (((rotation || 0) % 360) + 360) % 360;
}

/** The frame in the orientation the user sees it (native size swapped if needed). */
export function logicalFrame(nativeWidth, nativeHeight, orientation) {
  let w = nativeWidth;
  let h = nativeHeight;
  if (orientation === "portrait" && w > h) [w, h] = [h, w];
  else if (orientation === "landscape" && w < h) [w, h] = [h, w];
  return { width: w, height: h };
}

/** Largest frame-shaped rect that fits inside the image, centered ("cover"). */
export function coverRect(imgW, imgH, frameW, frameH) {
  const aspect = frameW / frameH;
  let w = imgW;
  let h = w / aspect;
  if (h > imgH) {
    h = imgH;
    w = h * aspect;
  }
  return { x: (imgW - w) / 2, y: (imgH - h) / 2, w, h };
}

/** Smallest frame-shaped rect that contains the whole image, centered ("fit"). */
export function fitRect(imgW, imgH, frameW, frameH) {
  const aspect = frameW / frameH;
  let w = imgW;
  let h = w / aspect;
  if (h < imgH) {
    h = imgH;
    w = h * aspect;
  }
  return { x: (imgW - w) / 2, y: (imgH - h) / 2, w, h };
}

export function normalizeRect(rect, imgW, imgH) {
  return { x: rect.x / imgW, y: rect.y / imgH, w: rect.w / imgW, h: rect.h / imgH };
}

export function denormalizeRect(rect, imgW, imgH) {
  return { x: rect.x * imgW, y: rect.y * imgH, w: rect.w * imgW, h: rect.h * imgH };
}

/**
 * Convert a pixel rect into the zoom / pan the image library expects for its
 * "custom" scale mode: the image is drawn at `zoom` times its size, with its
 * top-left corner at (panX, panY) in frame pixels. The rect already has the
 * frame's aspect ratio, so the width alone fixes the zoom.
 */
export function rectToZoomPan(rect, frameW) {
  const zoom = frameW / rect.w;
  return { zoom, panX: -rect.x * zoom, panY: -rect.y * zoom };
}

/** True when two rects are the same within a tolerance (in the rect's units). */
export function rectsEqual(a, b, tolerance = 0.5) {
  if (!a || !b) return false;
  return (
    Math.abs(a.x - b.x) <= tolerance &&
    Math.abs(a.y - b.y) <= tolerance &&
    Math.abs(a.w - b.w) <= tolerance &&
    Math.abs(a.h - b.h) <= tolerance
  );
}

/** True when the rect leaves part of the frame uncovered (letterbox bars). */
export function rectHasBars(rect, imgW, imgH, tolerance = 0.5) {
  return (
    rect.x < -tolerance ||
    rect.y < -tolerance ||
    rect.x + rect.w > imgW + tolerance ||
    rect.y + rect.h > imgH + tolerance
  );
}

/**
 * Keep a rect usable: bounded size, and positioned so that it stays inside the
 * image when it is smaller than the image, or keeps the image inside it when it
 * is larger (letterbox). Width and height are re-derived from the aspect ratio.
 */
export function clampRect(rect, imgW, imgH, aspect, { minW, maxW }) {
  let w = Math.min(Math.max(rect.w, minW), maxW);
  let h = w / aspect;
  const x = clampAxis(rect.x, w, imgW);
  const y = clampAxis(rect.y, h, imgH);
  return { x, y, w, h };
}

function clampAxis(pos, size, extent) {
  if (size <= extent) return Math.min(Math.max(pos, 0), extent - size);
  return Math.min(Math.max(pos, extent - size), 0);
}

/**
 * Size limits for a rect, in image pixels.
 *   minW  never enlarge the photo more than MAX_UPSCALE times (soft floor for
 *         tiny photos: the cover rect is always allowed)
 *   maxW  up to MAX_MARGIN times the fit rect, i.e. generous margins
 */
export const MAX_UPSCALE = 3;
export const MAX_MARGIN = 1.5;

export function rectLimits(imgW, imgH, frameW, frameH, imagePixelsPerFramePixel = 1) {
  const cover = coverRect(imgW, imgH, frameW, frameH);
  const fit = fitRect(imgW, imgH, frameW, frameH);
  const minW = Math.min(cover.w, (frameW / MAX_UPSCALE) * imagePixelsPerFramePixel);
  return { minW, maxW: fit.w * MAX_MARGIN };
}

/** How much the photo gets enlarged on the frame for a rect (1 = no enlargement). */
export function upscaleFactor(rect, frameW, imagePixelsPerFramePixel = 1) {
  return (frameW * imagePixelsPerFramePixel) / rect.w;
}

/** Rotate (multiple of 90 degrees) and optionally mirror a canvas. */
export function orientCanvas(canvas, rotation = 0, flipH = false) {
  let out = rotateImage(canvas, normalizeRotation(rotation));
  if (flipH) {
    const flipped = document.createElement("canvas");
    flipped.width = out.width;
    flipped.height = out.height;
    const ctx = flipped.getContext("2d");
    ctx.translate(out.width, 0);
    ctx.scale(-1, 1);
    ctx.drawImage(out, 0, 0);
    out = flipped;
  }
  return out;
}

/**
 * Downscale a canvas so its longest side is at most `maxSide`, halving in
 * steps so browsers don't alias. Returns the input untouched when it already
 * fits.
 */
export function downscaleCanvas(canvas, maxSide) {
  const longest = Math.max(canvas.width, canvas.height);
  if (longest <= maxSide) return canvas;
  const targetScale = maxSide / longest;
  let current = canvas;
  let w = canvas.width;
  let h = canvas.height;
  while (w * 0.5 > canvas.width * targetScale) {
    w = Math.round(w / 2);
    h = Math.round(h / 2);
    current = drawScaled(current, w, h);
  }
  const finalW = Math.round(canvas.width * targetScale);
  const finalH = Math.round(canvas.height * targetScale);
  if (finalW !== w || finalH !== h) current = drawScaled(current, finalW, finalH);
  return current;
}

function drawScaled(source, width, height) {
  const out = document.createElement("canvas");
  out.width = width;
  out.height = height;
  const ctx = out.getContext("2d");
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(source, 0, 0, width, height);
  return out;
}

/** Decode a File into an HTMLImageElement (the browser applies EXIF orientation). */
export function loadImageFile(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      URL.revokeObjectURL(url);
      resolve(img);
    };
    img.onerror = (e) => {
      URL.revokeObjectURL(url);
      reject(e);
    };
    img.src = url;
  });
}

/** Draw an image (or canvas) into a fresh canvas of the same size. */
export function toCanvas(source) {
  const canvas = document.createElement("canvas");
  canvas.width = source.naturalWidth || source.width;
  canvas.height = source.naturalHeight || source.height;
  canvas.getContext("2d").drawImage(source, 0, 0);
  return canvas;
}
