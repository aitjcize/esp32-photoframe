#!/usr/bin/env node

import { createCanvas, loadImage } from 'canvas';
import { Command } from 'commander';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { processImage, PALETTE_THEORETICAL } from './image-processor.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const DISPLAY_WIDTH = 800;
const DISPLAY_HEIGHT = 480;

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
    buffer.write('BM', offset); offset += 2;
    buffer.writeUInt32LE(fileSize, offset); offset += 4;
    buffer.writeUInt32LE(0, offset); offset += 4; // Reserved
    buffer.writeUInt32LE(headerSize, offset); offset += 4;
    
    // Info header (40 bytes)
    buffer.writeUInt32LE(infoHeaderSize, offset); offset += 4;
    buffer.writeInt32LE(width, offset); offset += 4;
    buffer.writeInt32LE(height, offset); offset += 4;
    buffer.writeUInt16LE(1, offset); offset += 2; // Planes
    buffer.writeUInt16LE(24, offset); offset += 2; // Bits per pixel (24-bit RGB)
    buffer.writeUInt32LE(0, offset); offset += 4; // Compression (none)
    buffer.writeUInt32LE(imageSize, offset); offset += 4;
    buffer.writeInt32LE(2835, offset); offset += 4; // X pixels per meter
    buffer.writeInt32LE(2835, offset); offset += 4; // Y pixels per meter
    buffer.writeUInt32LE(0, offset); offset += 4; // Colors used (0 = all colors)
    buffer.writeUInt32LE(0, offset); offset += 4; // Important colors
    
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
        const padding = rowSize - (width * 3);
        for (let i = 0; i < padding; i++) {
            buffer.writeUInt8(0, offset++);
        }
    }
    
    fs.writeFileSync(outputPath, buffer);
}

// Resize image with cover mode (scale and crop to fill)
function resizeImageCover(canvas, targetWidth, targetHeight) {
    const srcWidth = canvas.width;
    const srcHeight = canvas.height;
    
    // Calculate scale to cover (larger of the two ratios)
    const scaleX = targetWidth / srcWidth;
    const scaleY = targetHeight / srcHeight;
    const scale = Math.max(scaleX, scaleY);
    
    const scaledWidth = Math.round(srcWidth * scale);
    const scaledHeight = Math.round(srcHeight * scale);
    
    // Create temporary canvas for scaling
    const tempCanvas = createCanvas(scaledWidth, scaledHeight);
    const tempCtx = tempCanvas.getContext('2d');
    tempCtx.drawImage(canvas, 0, 0, scaledWidth, scaledHeight);
    
    // Crop to target size (center crop)
    const cropX = Math.round((scaledWidth - targetWidth) / 2);
    const cropY = Math.round((scaledHeight - targetHeight) / 2);
    
    const outputCanvas = createCanvas(targetWidth, targetHeight);
    const outputCtx = outputCanvas.getContext('2d');
    outputCtx.drawImage(tempCanvas, cropX, cropY, targetWidth, targetHeight, 0, 0, targetWidth, targetHeight);
    
    return outputCanvas;
}

// Rotate 90 degrees clockwise
function rotate90Clockwise(canvas) {
    const rotatedCanvas = createCanvas(canvas.height, canvas.width);
    const ctx = rotatedCanvas.getContext('2d');
    
    ctx.translate(canvas.height, 0);
    ctx.rotate(Math.PI / 2);
    ctx.drawImage(canvas, 0, 0);
    
    return rotatedCanvas;
}

async function processImageFile(inputPath, outputBmp, outputThumb, options) {
    console.log(`Processing: ${inputPath}`);
    
    // 1. Load image
    const img = await loadImage(inputPath);
    let canvas = createCanvas(img.width, img.height);
    let ctx = canvas.getContext('2d');
    ctx.drawImage(img, 0, 0);
    
    console.log(`  Original size: ${canvas.width}x${canvas.height}`);
    
    // Save original image for thumbnail generation (before any processing)
    const originalImg = img;
    
    // 2. Check if portrait and rotate (skip rotation if rendering measured for debugging)
    const isPortrait = canvas.height > canvas.width;
    if (isPortrait && !options.renderMeasured) {
        console.log(`  Portrait detected, rotating 90° clockwise`);
        canvas = rotate90Clockwise(canvas);
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
        console.log(`  Resizing to ${targetWidth}x${targetHeight} (cover mode: scale and crop)`);
        canvas = resizeImageCover(canvas, targetWidth, targetHeight);
    }
    
    // 4. Apply image processing
    ctx = canvas.getContext('2d');
    const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
    
    const params = {
        saturation: options.saturation,
        strength: options.scurveStrength,
        shadowBoost: options.scurveShadow,
        highlightCompress: options.scurveHighlight,
        midpoint: options.scurveMidpoint,
        colorMethod: 'rgb', // CLI always uses RGB for consistency
        renderMeasured: options.renderMeasured,
        processingMode: options.processingMode
    };
    
    if (params.processingMode === 'stock') {
        console.log(`  Using stock Waveshare algorithm (no S-curve, theoretical palette)`);
    } else {
        console.log(`  Using enhanced algorithm with S-curve tone mapping`);
        console.log(`  S-curve: strength=${params.strength}, shadow=${params.shadowBoost}, highlight=${params.highlightCompress}`);
        console.log(`  Saturation: ${params.saturation}`);
    }
    
    if (options.renderMeasured) {
        console.log(`  Rendering BMP with measured colors (darker output for preview)`);
    } else {
        console.log(`  Rendering BMP with theoretical colors (standard output)`);
    }
    
    processImage(imageData, params);
    ctx.putImageData(imageData, 0, 0);
    
    // 5. Write BMP
    console.log(`  Writing BMP: ${outputBmp}`);
    writeBMP(imageData, outputBmp);
    
    // 6. Generate thumbnail if requested (from original source, not processed image)
    if (options.generateThumbnail && outputThumb) {
        console.log(`  Generating thumbnail: ${outputThumb}`);
        
        // Create thumbnail from original image (unprocessed)
        // Determine thumbnail orientation based on original image
        const thumbWidth = isPortrait ? 96 : 160;
        const thumbHeight = isPortrait ? 160 : 96;
        
        const thumbCanvas = createCanvas(thumbWidth, thumbHeight);
        const thumbCtx = thumbCanvas.getContext('2d');
        
        // Scale original image to thumbnail size (cover mode)
        const srcWidth = originalImg.width;
        const srcHeight = originalImg.height;
        const scaleX = thumbWidth / srcWidth;
        const scaleY = thumbHeight / srcHeight;
        const scale = Math.max(scaleX, scaleY);
        
        const scaledWidth = Math.round(srcWidth * scale);
        const scaledHeight = Math.round(srcHeight * scale);
        const cropX = Math.round((scaledWidth - thumbWidth) / 2);
        const cropY = Math.round((scaledHeight - thumbHeight) / 2);
        
        // Draw scaled and cropped original image
        thumbCtx.drawImage(originalImg, 
            cropX / scale, cropY / scale, thumbWidth / scale, thumbHeight / scale,
            0, 0, thumbWidth, thumbHeight);
        
        const buffer = thumbCanvas.toBuffer('image/jpeg', { quality: 0.8 });
        fs.writeFileSync(outputThumb, buffer);
    }
    
    console.log(`Done!`);
}

// CLI setup
const program = new Command();

program
    .name('photoframe-process')
    .description('ESP32 PhotoFrame image processing CLI')
    .version('1.0.0')
    .argument('<input>', 'Input image file (JPEG/PNG)')
    .option('-o, --output-dir <dir>', 'Output directory', '.')
    .option('--suffix <suffix>', 'Suffix to add to output filename', '')
    .option('--no-thumbnail', 'Skip thumbnail generation')
    .option('--scurve-strength <value>', 'S-curve overall strength (0.0-1.0)', parseFloat, 0.9)
    .option('--scurve-shadow <value>', 'S-curve shadow boost (0.0-1.0)', parseFloat, 0.0)
    .option('--scurve-highlight <value>', 'S-curve highlight compress (0.5-3.0)', parseFloat, 1.7)
    .option('--scurve-midpoint <value>', 'S-curve midpoint (0.3-0.7)', parseFloat, 0.5)
    .option('--saturation <value>', 'Saturation multiplier (1.0=normal, >1.0=more vibrant)', parseFloat, 1.2)
    .option('--render-measured', 'Render BMP with measured palette colors (darker output for preview)')
    .option('--processing-mode <mode>', 'Processing algorithm: enhanced (default, with S-curve) or stock (Waveshare original)', 'enhanced')
    .action(async (input, options) => {
        try {
            const inputPath = path.resolve(input);
            if (!fs.existsSync(inputPath)) {
                console.error(`Error: Input file not found: ${inputPath}`);
                process.exit(1);
            }
            
            const outputDir = path.resolve(options.outputDir);
            if (!fs.existsSync(outputDir)) {
                fs.mkdirSync(outputDir, { recursive: true });
            }
            
            const baseName = path.basename(input, path.extname(input));
            const suffix = options.suffix || '';
            const outputBmp = path.join(outputDir, `${baseName}${suffix}.bmp`);
            const outputThumb = options.thumbnail ? path.join(outputDir, `${baseName}${suffix}.jpg`) : null;
            
            await processImageFile(inputPath, outputBmp, outputThumb, {
                generateThumbnail: options.thumbnail,
                scurveStrength: options.scurveStrength,
                scurveShadow: options.scurveShadow,
                scurveHighlight: options.scurveHighlight,
                scurveMidpoint: options.scurveMidpoint,
                saturation: options.saturation,
                renderMeasured: options.renderMeasured,
                processingMode: options.processingMode
            });
        } catch (error) {
            console.error(`Error processing image: ${error.message}`);
            console.error(error.stack);
            process.exit(1);
        }
    });

program.parse();
