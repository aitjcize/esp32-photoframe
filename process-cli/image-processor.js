// Image processing functions for S-curve tone mapping and dithering
// This file is shared between the webapp and CLI

// Measured palette - actual displayed colors from e-paper
const PALETTE_MEASURED = [
    [2, 2, 2],        // Black
    [190, 190, 190],  // White
    [205, 202, 0],    // Yellow
    [135, 19, 0],     // Red
    [0, 0, 0],        // Reserved (not used)
    [5, 64, 158],     // Blue
    [39, 102, 60]     // Green
];

// Theoretical palette - for BMP output
const PALETTE_THEORETICAL = [
    [0, 0, 0],        // Black
    [255, 255, 255],  // White
    [255, 255, 0],    // Yellow
    [255, 0, 0],      // Red
    [0, 0, 0],        // Reserved
    [0, 0, 255],      // Blue
    [0, 255, 0]       // Green
];

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
        const x = c * (1 - Math.abs((h * 6) % 2 - 1));
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

function applyScurveTonemap(imageData, strength, shadowBoost, highlightCompress, midpoint) {
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
                result = midpoint + Math.pow(highlightVal, 1.0 + strength * highlightCompress) * (1.0 - midpoint);
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
    const y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750;
    const z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041;
    
    return [x * 100, y * 100, z * 100];
}

function xyzToLab(x, y, z) {
    x = x / 95.047;
    y = y / 100.000;
    z = z / 108.883;
    
    x = x > 0.008856 ? Math.pow(x, 1/3) : (7.787 * x) + 16/116;
    y = y > 0.008856 ? Math.pow(y, 1/3) : (7.787 * y) + 16/116;
    z = z > 0.008856 ? Math.pow(z, 1/3) : (7.787 * z) + 16/116;
    
    const L = (116 * y) - 16;
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
function findClosestColor(r, g, b, method = 'rgb', palette = PALETTE_MEASURED) {
    return method === 'lab' ? findClosestColorLAB(r, g, b) : findClosestColorRGB(r, g, b, palette);
}

function applyFloydSteinbergDither(imageData, method = 'rgb', outputPalette = PALETTE_MEASURED, ditherPalette = PALETTE_MEASURED) {
    
    const width = imageData.width;
    const height = imageData.height;
    const data = imageData.data;
    
    // Create error buffer
    const errors = new Array(width * height * 3).fill(0);
    
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
            const colorIdx = findClosestColor(oldR, oldG, oldB, method, ditherPalette);
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
            
            // Distribute error to neighboring pixels (Floyd-Steinberg)
            if (x + 1 < width) {
                const nextIdx = (y * width + (x + 1)) * 3;
                errors[nextIdx] += errR * 7 / 16;
                errors[nextIdx + 1] += errG * 7 / 16;
                errors[nextIdx + 2] += errB * 7 / 16;
            }
            
            if (y + 1 < height) {
                if (x > 0) {
                    const nextIdx = ((y + 1) * width + (x - 1)) * 3;
                    errors[nextIdx] += errR * 3 / 16;
                    errors[nextIdx + 1] += errG * 3 / 16;
                    errors[nextIdx + 2] += errB * 3 / 16;
                }
                
                const nextIdx = ((y + 1) * width + x) * 3;
                errors[nextIdx] += errR * 5 / 16;
                errors[nextIdx + 1] += errG * 5 / 16;
                errors[nextIdx + 2] += errB * 5 / 16;
                
                if (x + 1 < width) {
                    const nextIdx = ((y + 1) * width + (x + 1)) * 3;
                    errors[nextIdx] += errR * 1 / 16;
                    errors[nextIdx + 1] += errG * 1 / 16;
                    errors[nextIdx + 2] += errB * 1 / 16;
                }
            }
        }
    }
}

function processImage(imageData, params) {
    // Processing mode: 'stock' (Waveshare original) or 'enhanced' (our algorithm with S-curve)
    const mode = params.processingMode || 'enhanced';
    
    if (mode === 'stock') {
        // Stock Waveshare algorithm: no S-curve, theoretical palette for dithering
        // But render with measured palette for accurate preview
        const outputPalette = params.renderMeasured ? PALETTE_MEASURED : PALETTE_THEORETICAL;
        applyFloydSteinbergDither(imageData, 'rgb', outputPalette, PALETTE_THEORETICAL);
    } else {
        // Enhanced algorithm with S-curve tone mapping
        
        // 1. Apply saturation
        if (params.saturation !== 1.0) {
            applySaturation(imageData, params.saturation);
        }
        
        // 2. Apply S-curve tone mapping
        applyScurveTonemap(
            imageData,
            params.strength,
            params.shadowBoost,
            params.highlightCompress,
            params.midpoint
        );
        
        // 3. Apply Floyd-Steinberg dithering with measured palette for accurate error diffusion
        const outputPalette = params.renderMeasured ? PALETTE_MEASURED : PALETTE_THEORETICAL;
        applyFloydSteinbergDither(imageData, params.colorMethod, outputPalette, PALETTE_MEASURED);
    }
}

// Export for Node.js ES modules
export {
    PALETTE_MEASURED,
    PALETTE_THEORETICAL,
    applySaturation,
    applyScurveTonemap,
    applyFloydSteinbergDither,
    processImage
};