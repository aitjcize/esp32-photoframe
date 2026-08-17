/**
 * HTTP Server for image serving mode
 * Provides endpoints for serving processed images and thumbnails
 */

import http from "http";
import url from "url";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";
import { createCanvas } from "canvas";
import { processImagePipeline } from "./utils.js";
import {
  generateThumbnail,
  createPNG,
  createBMP,
  createEPDGZ,
  getDefaultParams,
  getPreset,
} from "@aitjcize/epaper-image-convert";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// Get default parameters from the library
const DEFAULT_PARAMS = getDefaultParams();

// Display dimensions
const DEFAULT_DISPLAY_WIDTH = 800;
const DEFAULT_DISPLAY_HEIGHT = 480;

/**
 * Create and start an HTTP server for serving processed images
 *
 * @param {string} albumDir - Directory containing image albums
 * @param {number} port - Port to listen on
 * @param {string} serveFormat - Output format (jpg, png, bmp)
 * @param {Object} devicePalette - Optional device color palette
 * @param {Object} processingOptions - Resolved processing parameters
 * @param {Object} options - Additional options (silent, etc.)
 * @returns {Promise<http.Server>} The HTTP server instance
 */

export async function createImageServer(
  albumDir,
  port,
  serveFormat,
  devicePalette,
  processingOptions,
  options = {},
) {
  // Validate serve format
  const validFormats = ["epdgz", "png", "jpg", "bmp"];
  if (!validFormats.includes(serveFormat)) {
    throw new Error(
      `Invalid serve format "${serveFormat}". Must be one of: ${validFormats.join(", ")}`,
    );
  }

  // Use pre-resolved processing params
  const processingParams = { ...processingOptions };
  const baseOptions = processingOptions || {};

  // Scan albums and collect all images
  const albums = {};
  const allImages = [];

  if (!options.silent) {
    console.log(`Scanning album directory: ${albumDir}`);
  }

  const entries = fs.readdirSync(albumDir, { withFileTypes: true });
  for (const entry of entries) {
    if (!entry.isDirectory()) continue;

    const albumName = entry.name;
    const albumPath = path.join(albumDir, albumName);
    const images = [];

    const files = fs.readdirSync(albumPath);
    for (const file of files) {
      const ext = path.extname(file).toLowerCase();
      if ([".png", ".jpg", ".jpeg", ".heic"].includes(ext)) {
        images.push({
          name: file,
          path: path.join(albumPath, file),
          album: albumName,
        });
      }
    }

    if (images.length > 0) {
      albums[albumName] = images;
      allImages.push(...images);
      if (!options.silent) {
        console.log(`  Album "${albumName}": ${images.length} images`);
      }
    }
  }

  if (allImages.length === 0) {
    throw new Error("No images found in album directory");
  }

  if (!options.silent) {
    console.log(
      `Total images: ${allImages.length} across ${Object.keys(albums).length} albums`,
    );
  }

  // Cache for generated thumbnails
  const thumbnailCache = new Map();

  const server = http.createServer(async (req, res) => {
    const parsedUrl = url.parse(req.url, true);
    const pathname = parsedUrl.pathname;

    // CORS headers
    res.setHeader("Access-Control-Allow-Origin", "*");
    res.setHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    res.setHeader("Access-Control-Allow-Headers", "Content-Type");

    if (req.method === "OPTIONS") {
      res.writeHead(200);
      res.end();
      return;
    }

    // GET /image - serve random image
    if (pathname === "/image" && req.method === "GET") {
      const randomIndex = Math.floor(Math.random() * allImages.length);
      const image = allImages[randomIndex];

      try {
        // Get display dimensions from headers or use defaults
        let width =
          parseInt(req.headers["x-display-width"]) ||
          baseOptions.displayWidth ||
          DEFAULT_DISPLAY_WIDTH;
        let height =
          parseInt(req.headers["x-display-height"]) ||
          baseOptions.displayHeight ||
          DEFAULT_DISPLAY_HEIGHT;

        // X-Display-Orientation indicates how the content should be oriented
        // but we should NOT swap the physical display dimensions - the device
        // sends its actual display size and expects the BMP to match those dimensions
        const orientation = req.headers["x-display-orientation"];
        if (!options.silent) {
          console.log(
            `Processing for display: ${width}x${height} (${orientation || "default"})`,
          );
        }

        // Merge processing settings from headers if present
        let currentProcessingParams = { ...processingParams };
        if (req.headers["x-processing-settings"]) {
          try {
            const headerSettings = JSON.parse(
              req.headers["x-processing-settings"],
            );
            currentProcessingParams = {
              ...currentProcessingParams,
              ...headerSettings,
            };
          } catch (e) {
            console.error(
              `Failed to parse X-Processing-Settings header: ${e.message}`,
            );
          }
        }

        // Use custom palette from header if present
        let currentDevicePalette = devicePalette;
        if (req.headers["x-color-palette"]) {
          try {
            currentDevicePalette = JSON.parse(req.headers["x-color-palette"]);
          } catch (e) {
            console.error(
              `Failed to parse X-Color-Palette header: ${e.message}`,
            );
          }
        }

        // Process image through pipeline
        const { canvas: processedCanvas, originalCanvas } =
          await processImagePipeline(
            image.path,
            currentProcessingParams,
            width,
            height,
            currentDevicePalette,
            {
              verbose: options.verbose,
              skipDithering: serveFormat === "jpg",
              // The device reports how it is mounted; without this a
              // portrait frame would be laid out (and letterboxed) in
              // landscape
              orientation: orientation || "landscape",
              // Layout comes from the device's synced settings, not the
              // tone/dither params object the pipeline reads separately
              scaleMode: currentProcessingParams.scaleMode || "cover",
              backgroundColor:
                currentProcessingParams.backgroundColor || "white",
            },
          );

        // Cache the thumbnail under the full render signature (panel
        // dimensions, orientation, layout) keyed on the unique file PATH:
        // concurrent frames with different geometry or layout must each get
        // a thumbnail matching their own rendering, and identical basenames
        // in different albums must not collide
        const thumbSig = `${width}x${height}|${orientation || "landscape"}|${
          currentProcessingParams.scaleMode || "cover"
        }-${currentProcessingParams.backgroundColor || "white"}`;
        const thumbCacheKey = `${image.path}|${thumbSig}`;
        if (!thumbnailCache.has(thumbCacheKey)) {
          const thumbCanvas = generateThumbnail(
            originalCanvas,
            400,
            createCanvas,
          );
          const thumbBuffer = thumbCanvas.toBuffer("image/jpeg", {
            quality: 0.8,
          });
          // Crude bound: the signature space is per-panel finite, but keep a
          // hard cap so the cache can never grow without limit
          if (thumbnailCache.size >= 200) {
            thumbnailCache.clear();
          }
          thumbnailCache.set(thumbCacheKey, thumbBuffer);
        }

        // Convert to requested format
        let imageBuffer;
        let contentType;

        if (serveFormat === "epdgz") {
          contentType = "application/octet-stream";
          imageBuffer = await createEPDGZ(processedCanvas);
        } else if (serveFormat === "jpg") {
          contentType = "image/jpeg";
          imageBuffer = processedCanvas.toBuffer("image/jpeg", {
            quality: 0.95,
          });
        } else if (serveFormat === "png") {
          contentType = "image/png";
          imageBuffer = await createPNG(processedCanvas);
        } else if (serveFormat === "bmp") {
          contentType = "image/bmp";
          imageBuffer = createBMP(processedCanvas);
        }

        // Set headers
        // Compact index id (allImages is fixed at startup): the absolute
        // path would leak the filesystem layout and can overflow the
        // firmware's 512-byte URL buffer on deep album trees
        const thumbnailUrl = `http://${req.headers.host}/thumbnail?file=${encodeURIComponent(image.name)}&id=${allImages.indexOf(image)}&sig=${encodeURIComponent(thumbSig)}`;
        res.setHeader("X-Thumbnail-URL", thumbnailUrl);
        res.setHeader("Content-Type", contentType);
        res.setHeader("Content-Length", imageBuffer.length);
        res.writeHead(200);
        res.end(imageBuffer);

        if (!options.silent) {
          console.log(
            `[${new Date().toISOString()}] Served: ${image.album}/${image.name} (${serveFormat.toUpperCase()}) [Thumbnail: ${thumbnailUrl}]`,
          );
        }
      } catch (error) {
        console.error(`Error processing image: ${error.message}`);
        res.writeHead(500);
        res.end("Internal Server Error");
      }
      return;
    }

    // GET /thumbnail - serve thumbnail
    if (pathname === "/thumbnail" && req.method === "GET") {
      const fileName = parsedUrl.query.file;

      // The signature from the /image response's X-Thumbnail-URL:
      // "WxH|orientation|scale-bg". Every component is validated against an
      // allowlist -- arbitrary values would each mint a new cache key.
      const sigParts = (parsedUrl.query.sig || "").split("|");
      const dimsMatch = /^(\d{2,4})x(\d{2,4})$/.exec(sigParts[0] || "");
      // Clamp to plausible panel sizes: unchecked values would flow into
      // canvas allocations
      const dimsValid =
        dimsMatch &&
        parseInt(dimsMatch[1], 10) <= 4096 &&
        parseInt(dimsMatch[2], 10) <= 4096;
      const thumbWidth = dimsValid
        ? parseInt(dimsMatch[1], 10)
        : DEFAULT_DISPLAY_WIDTH;
      const thumbHeight = dimsValid
        ? parseInt(dimsMatch[2], 10)
        : DEFAULT_DISPLAY_HEIGHT;
      const thumbOrientation = ["landscape", "portrait"].includes(sigParts[1])
        ? sigParts[1]
        : "landscape";
      const layoutParts = (sigParts[2] || "").split("-");
      const thumbScaleMode =
        layoutParts[0] === "fit" || layoutParts[0] === "cover"
          ? layoutParts[0]
          : processingParams.scaleMode || "cover";
      const thumbBackground = ["white", "black"].includes(layoutParts[1])
        ? layoutParts[1]
        : processingParams.backgroundColor || "white";
      const thumbSig = `${thumbWidth}x${thumbHeight}|${thumbOrientation}|${thumbScaleMode}-${thumbBackground}`;

      if (!fileName) {
        res.writeHead(400);
        res.end("Missing file parameter");
        return;
      }

      try {
        // Resolve by the compact index id when given (identical basenames
        // can exist in different albums); fall back to the name for manual
        // requests
        const qId = parseInt(parsedUrl.query.id, 10);
        const image =
          (Number.isInteger(qId) && qId >= 0 && allImages[qId]) ||
          allImages.find((img) => img.name === fileName);
        if (!image) {
          res.writeHead(404);
          res.end("Image not found");
          return;
        }
        const thumbKey = `${image.path}|${thumbSig}`;

        // Check cache
        if (thumbnailCache.has(thumbKey)) {
          const thumbBuffer = thumbnailCache.get(thumbKey);
          res.setHeader("Content-Type", "image/jpeg");
          res.setHeader("Content-Length", thumbBuffer.length);
          res.writeHead(200);
          res.end(thumbBuffer);

          if (!options.silent) {
            console.log(
              `[${new Date().toISOString()}] Served cached thumbnail: ${fileName}`,
            );
          }
          return;
        }

        // Generate with the validated signature's geometry and layout
        const { originalCanvas } = await processImagePipeline(
          image.path,
          processingParams,
          thumbWidth,
          thumbHeight,
          devicePalette,
          {
            skipDithering: true,
            orientation: thumbOrientation,
            scaleMode: thumbScaleMode,
            backgroundColor: thumbBackground,
          },
        );

        const thumbCanvas = generateThumbnail(
          originalCanvas,
          400,
          createCanvas,
        );
        const thumbBuffer = thumbCanvas.toBuffer("image/jpeg", {
          quality: 0.8,
        });

        if (thumbnailCache.size >= 200) {
          thumbnailCache.clear();
        }
        thumbnailCache.set(thumbKey, thumbBuffer);

        res.setHeader("Content-Type", "image/jpeg");
        res.setHeader("Content-Length", thumbBuffer.length);
        res.writeHead(200);
        res.end(thumbBuffer);

        if (!options.silent) {
          console.log(
            `[${new Date().toISOString()}] Generated and served thumbnail: ${image.album}/${image.name}`,
          );
        }
      } catch (error) {
        console.error(`Error generating thumbnail: ${error.message}`);
        res.writeHead(500);
        res.end("Internal Server Error");
      }
      return;
    }

    // GET /status - server status
    if (pathname === "/status" && req.method === "GET") {
      const status = {
        totalImages: allImages.length,
        albums: Object.keys(albums).length,
        serveFormat: serveFormat,
      };
      res.setHeader("Content-Type", "application/json");
      res.writeHead(200);
      res.end(JSON.stringify(status, null, 2));
      return;
    }

    // 404
    res.writeHead(404);
    res.end("Not Found");
  });

  // Return server instance
  return new Promise((resolve, reject) => {
    server.listen(port, () => {
      if (!options.silent) {
        console.log(`\nImage server running on http://localhost:${port}`);
        console.log(
          `  Image endpoint: http://localhost:${port}/image (format: ${serveFormat.toUpperCase()})`,
        );
        console.log(
          `  Thumbnail endpoint: http://localhost:${port}/thumbnail?file=<filename>`,
        );
        console.log(`  Status endpoint: http://localhost:${port}/status`);
        console.log(`\nPress Ctrl+C to stop\n`);
      }
      resolve(server);
    });

    server.on("error", (err) => {
      if (err.code === "EADDRINUSE") {
        if (!options.silent) {
          console.error(`\nError: Port ${port} is already in use.`);
          console.error(
            `Please try a different port with --serve-port <port>\n`,
          );
        }
        reject(err);
      } else {
        if (!options.silent) {
          console.error(`\nServer error: ${err.message}\n`);
        }
        reject(err);
      }
    });
  });
}
