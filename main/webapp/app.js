import { processImage } from './image-processor.js';

const API_BASE = '';

let currentImages = [];
let selectedImage = null;

async function loadBatteryStatus() {
    try {
        const response = await fetch(`${API_BASE}/api/battery`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            return;
        }
        const data = await response.json();
        
        const batteryDiv = document.getElementById('batteryStatus');
        
        if (!data.connected) {
            batteryDiv.innerHTML = '<span class="battery-disconnected">🔌 No Battery</span>';
            return;
        }
        
        const percent = data.percent;
        const voltage = data.voltage;
        const charging = data.charging;
        
        let batteryIcon = '🔋';
        let batteryClass = 'battery-normal';
        
        if (charging) {
            batteryIcon = '⚡';
            batteryClass = 'battery-charging';
        } else if (percent < 20) {
            batteryClass = 'battery-low';
        } else if (percent < 50) {
            batteryClass = 'battery-medium';
        }
        
        batteryDiv.innerHTML = `
            <span class="${batteryClass}">
                ${batteryIcon} ${percent}% (${(voltage / 1000).toFixed(2)}V)
                ${charging ? ' Charging' : ''}
            </span>
        `;
    } catch (error) {
        // Silently fail if API not available (standalone mode)
        console.log('Battery API not available (standalone mode)');
    }
}

async function loadImages() {
    try {
        const response = await fetch(`${API_BASE}/api/images`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            console.log('Images API not available (standalone mode)');
            return;
        }
        const data = await response.json();
        currentImages = data.images || [];
        displayImages();
    } catch (error) {
        console.log('Failed to load images (firmware may be busy):', error);
    }
}

function displayImages() {
    const imageList = document.getElementById('imageList');
    
    if (currentImages.length === 0) {
        imageList.innerHTML = '<p class="loading">No images found. Upload some images to get started!</p>';
        return;
    }
    
    imageList.innerHTML = '';
    
    currentImages.forEach(image => {
        const item = document.createElement('div');
        item.className = 'image-item';
        
        const thumbnail = document.createElement('img');
        thumbnail.className = 'image-thumbnail';
        // Convert .bmp to .jpg for thumbnail
        const thumbnailName = image.name.replace(/\.bmp$/i, '.jpg');
        thumbnail.src = `${API_BASE}/api/image?name=${encodeURIComponent(thumbnailName)}`;
        thumbnail.alt = image.name;
        thumbnail.loading = 'lazy';
        
        const info = document.createElement('div');
        info.className = 'image-info';
        
        const name = document.createElement('div');
        name.className = 'image-name';
        name.textContent = image.name;
        
        const size = document.createElement('div');
        size.className = 'image-size';
        size.textContent = formatFileSize(image.size);
        
        info.appendChild(name);
        info.appendChild(size);
        
        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'delete-btn';
        deleteBtn.textContent = '×';
        deleteBtn.title = 'Delete image';
        deleteBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            deleteImage(image.name);
        });
        
        item.appendChild(thumbnail);
        item.appendChild(info);
        item.appendChild(deleteBtn);
        
        item.addEventListener('click', () => selectImage(image.name, item));
        
        imageList.appendChild(item);
    });
}

function formatFileSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

async function deleteImage(filename) {
    if (!confirm(`Are you sure you want to delete "${filename}"?`)) {
        return;
    }
    
    try {
        const response = await fetch(`${API_BASE}/api/delete`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ filename })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            console.log('Image deleted:', filename);
            loadImages(); // Reload the image list
        } else {
            alert('Failed to delete image');
        }
    } catch (error) {
        console.error('Error deleting image:', error);
        alert('Error deleting image');
    }
}

let isDisplaying = false;

async function selectImage(filename, element) {
    if (isDisplaying) {
        alert('Please wait for the current display operation to complete');
        return;
    }
    
    // Show confirmation dialog
    const confirmed = confirm(`Display "${filename}" on the e-paper screen?\n\nThis will take approximately 40 seconds to update.`);
    if (!confirmed) {
        return;
    }
    
    // Update selection
    document.querySelectorAll('.image-item').forEach(item => {
        item.classList.remove('selected');
    });
    element.classList.add('selected');
    selectedImage = filename;
    
    isDisplaying = true;
    
    // Show loading indicator
    const displayStatus = document.getElementById('displayStatus');
    displayStatus.style.display = 'block';
    
    try {
        const response = await fetch(`${API_BASE}/api/display`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ filename })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            console.log('Image displayed:', filename);
        } else if (data.status === 'busy') {
            alert('Display is currently updating, please wait');
        } else {
            alert('Failed to display image');
        }
    } catch (error) {
        console.error('Error displaying image:', error);
        alert('Error displaying image');
    } finally {
        // Hide loading indicator and re-enable display
        displayStatus.style.display = 'none';
        isDisplaying = false;
    }
}

// Global state for image processing
let currentImageFile = null;
let currentImageCanvas = null;
let originalImageData = null; // Store original unprocessed image data
let currentParams = {
    strength: 0.9,
    shadowBoost: 0.0,
    highlightCompress: 1.7,
    midpoint: 0.5,
    saturation: 1.2,
    colorMethod: 'rgb',  // 'rgb' or 'lab'
    renderMeasured: true,  // true = measured (darker) colors matching e-paper display
    processingMode: 'enhanced'  // 'stock' (Waveshare original) or 'enhanced' (our algorithm with S-curve)
};

document.getElementById('fileInput').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    
    const fileName = file.name;
    document.getElementById('fileName').textContent = `Selected: ${fileName}`;
    
    // Store the file and show preview
    currentImageFile = file;
    
    // Hide upload area, show preview area
    document.getElementById('uploadArea').style.display = 'none';
    document.getElementById('previewArea').style.display = 'block';
    
    // Ensure controls and buttons are visible (in case they were hidden from previous upload)
    document.querySelector('.button-group').style.display = 'flex';
    document.querySelector('.controls-grid').style.display = 'grid';
    document.getElementById('uploadProgress').style.display = 'none';
    
    await loadImagePreview(file);
});

// Drag and drop support
const uploadArea = document.querySelector('.upload-area');

uploadArea.addEventListener('dragover', (e) => {
    e.preventDefault();
    e.stopPropagation();
    uploadArea.classList.add('drag-over');
});

uploadArea.addEventListener('dragleave', (e) => {
    e.preventDefault();
    e.stopPropagation();
    uploadArea.classList.remove('drag-over');
});

uploadArea.addEventListener('drop', async (e) => {
    e.preventDefault();
    e.stopPropagation();
    uploadArea.classList.remove('drag-over');
    
    const files = e.dataTransfer.files;
    if (files.length > 0) {
        const file = files[0];
        // Check if it's a JPEG file
        if (file.type === 'image/jpeg' || file.name.toLowerCase().endsWith('.jpg') || file.name.toLowerCase().endsWith('.jpeg')) {
            const fileInput = document.getElementById('fileInput');
            fileInput.files = files;
            document.getElementById('fileName').textContent = `Selected: ${file.name}`;
            
            // Store the file and show preview
            currentImageFile = file;
            await loadImagePreview(file);
        } else {
            alert('Please drop a JPG/JPEG image file');
        }
    }
});

async function resizeImage(file, maxWidth, maxHeight, quality) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
            const img = new Image();
            img.onload = () => {
                const canvas = document.createElement('canvas');
                const origWidth = img.width;
                const origHeight = img.height;
                
                // Determine if portrait or landscape
                const isPortrait = origHeight > origWidth;
                
                // Set canvas to exact display dimensions
                if (isPortrait) {
                    canvas.width = maxHeight;  // 480
                    canvas.height = maxWidth;  // 800
                } else {
                    canvas.width = maxWidth;   // 800
                    canvas.height = maxHeight; // 480
                }
                
                // Calculate scale to COVER (fill) the canvas
                const scaleX = canvas.width / origWidth;
                const scaleY = canvas.height / origHeight;
                const scale = Math.max(scaleX, scaleY);
                
                const scaledWidth = Math.round(origWidth * scale);
                const scaledHeight = Math.round(origHeight * scale);
                
                // Center and crop
                const offsetX = (canvas.width - scaledWidth) / 2;
                const offsetY = (canvas.height - scaledHeight) / 2;
                
                const ctx = canvas.getContext('2d');
                ctx.drawImage(img, offsetX, offsetY, scaledWidth, scaledHeight);
                
                canvas.toBlob((blob) => {
                    resolve(blob);
                }, 'image/jpeg', quality);
            };
            img.onerror = reject;
            img.src = e.target.result;
        };
        reader.onerror = reject;
        reader.readAsDataURL(file);
    });
}

async function loadImagePreview(file) {
    const canvas = document.getElementById('previewCanvas');
    const statusDiv = document.getElementById('uploadStatus');
    
    // Clear any previous status
    statusDiv.textContent = '';
    statusDiv.className = '';
    
    try {
        // Load image and resize to display size
        const img = await loadImage(file);
        
        // Determine if portrait or landscape
        const isPortrait = img.height > img.width;
        
        // Set canvas to display dimensions (800x480 or 480x800)
        if (isPortrait) {
            canvas.width = 480;
            canvas.height = 800;
        } else {
            canvas.width = 800;
            canvas.height = 480;
        }
        
        // Draw image with cover mode (fill and crop)
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        const scaleX = canvas.width / img.width;
        const scaleY = canvas.height / img.height;
        const scale = Math.max(scaleX, scaleY);
        
        const scaledWidth = img.width * scale;
        const scaledHeight = img.height * scale;
        const offsetX = (canvas.width - scaledWidth) / 2;
        const offsetY = (canvas.height - scaledHeight) / 2;
        
        ctx.drawImage(img, offsetX, offsetY, scaledWidth, scaledHeight);
        
        // Store canvas and original image data
        currentImageCanvas = canvas;
        originalImageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
        
        // Apply initial processing
        updatePreview();
    } catch (error) {
        console.error('Error loading preview:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error loading image preview';
    }
}

function loadImage(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
            const img = new Image();
            img.onload = () => resolve(img);
            img.onerror = reject;
            img.src = e.target.result;
        };
        reader.onerror = reject;
        reader.readAsDataURL(file);
    });
}

function drawCurveVisualization() {
    const canvas = document.getElementById('curveCanvas');
    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;
    const padding = 40;
    const graphWidth = width - 2 * padding;
    const graphHeight = height - 2 * padding;
    
    // Clear canvas
    ctx.fillStyle = 'white';
    ctx.fillRect(0, 0, width, height);
    
    // Draw axes
    ctx.strokeStyle = '#333';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(padding, padding);
    ctx.lineTo(padding, height - padding);
    ctx.lineTo(width - padding, height - padding);
    ctx.stroke();
    
    // Draw axis labels
    ctx.fillStyle = '#333';
    ctx.font = '12px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Input', width / 2, height - 10);
    ctx.save();
    ctx.translate(15, height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText('Output', 0, 0);
    ctx.restore();
    
    // Draw grid lines
    ctx.strokeStyle = '#e0e0e0';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
        const x = padding + (graphWidth * i / 4);
        const y = padding + (graphHeight * i / 4);
        
        // Vertical grid lines
        ctx.beginPath();
        ctx.moveTo(x, padding);
        ctx.lineTo(x, height - padding);
        ctx.stroke();
        
        // Horizontal grid lines
        ctx.beginPath();
        ctx.moveTo(padding, y);
        ctx.lineTo(width - padding, y);
        ctx.stroke();
    }
    
    // Draw linear reference (y=x)
    ctx.strokeStyle = '#ccc';
    ctx.lineWidth = 1;
    ctx.setLineDash([5, 5]);
    ctx.beginPath();
    ctx.moveTo(padding, height - padding);
    ctx.lineTo(width - padding, padding);
    ctx.stroke();
    ctx.setLineDash([]);
    
    // Draw S-curve
    ctx.strokeStyle = '#667eea';
    ctx.lineWidth = 3;
    ctx.beginPath();
    
    const { strength, shadowBoost, highlightCompress, midpoint } = currentParams;
    
    for (let i = 0; i <= graphWidth; i++) {
        const normalized = i / graphWidth;
        let result;
        
        if (normalized <= midpoint) {
            // Shadow region
            const shadowVal = normalized / midpoint;
            result = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
        } else {
            // Highlight region
            const highlightVal = (normalized - midpoint) / (1.0 - midpoint);
            result = midpoint + Math.pow(highlightVal, 1.0 + strength * highlightCompress) * (1.0 - midpoint);
        }
        
        const x = padding + i;
        const y = height - padding - (result * graphHeight);
        
        if (i === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    }
    
    ctx.stroke();
    
    // Draw midpoint indicator
    ctx.strokeStyle = '#ff6b6b';
    ctx.lineWidth = 2;
    ctx.setLineDash([3, 3]);
    const midX = padding + midpoint * graphWidth;
    ctx.beginPath();
    ctx.moveTo(midX, padding);
    ctx.lineTo(midX, height - padding);
    ctx.stroke();
    ctx.setLineDash([]);
    
    // Midpoint label
    ctx.fillStyle = '#ff6b6b';
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('midpoint', midX, padding - 5);
}

function updatePreview() {
if (!currentImageCanvas || !originalImageData) return;
    
// Update curve visualization
drawCurveVisualization();
    
// Create a copy of the original image data
const tempCanvas = document.createElement('canvas');
tempCanvas.width = originalImageData.width;
tempCanvas.height = originalImageData.height;
const tempCtx = tempCanvas.getContext('2d', { willReadFrequently: true });
    
// Copy original image data
const imageDataCopy = tempCtx.createImageData(originalImageData.width, originalImageData.height);
imageDataCopy.data.set(originalImageData.data);
    
// Apply image processing to the ImageData copy
processImage(imageDataCopy, currentParams);
    
// Put processed data back to temp canvas
tempCtx.putImageData(imageDataCopy, 0, 0);
    
// Draw processed image to preview canvas
const previewCanvas = document.getElementById('previewCanvas');
const ctx = previewCanvas.getContext('2d');
ctx.drawImage(tempCanvas, 0, 0);
}

// Parameter change handlers
document.getElementById('scurveStrength').addEventListener('input', (e) => {
    currentParams.strength = parseFloat(e.target.value);
    document.getElementById('strengthValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveShadow').addEventListener('input', (e) => {
    currentParams.shadowBoost = parseFloat(e.target.value);
    document.getElementById('shadowValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveHighlight').addEventListener('input', (e) => {
    currentParams.highlightCompress = parseFloat(e.target.value);
    document.getElementById('highlightValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveMidpoint').addEventListener('input', (e) => {
    currentParams.midpoint = parseFloat(e.target.value);
    document.getElementById('midpointValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('saturation').addEventListener('input', (e) => {
    currentParams.saturation = parseFloat(e.target.value);
    document.getElementById('saturationValue').textContent = e.target.value;
    updatePreview();
});

// Color matching method radio buttons
document.querySelectorAll('input[name="colorMethod"]').forEach(radio => {
    radio.addEventListener('change', (e) => {
        currentParams.colorMethod = e.target.value;
        updatePreview();
    });
});

// Processing mode radio buttons
document.querySelectorAll('input[name="processingMode"]').forEach(radio => {
    radio.addEventListener('change', (e) => {
        currentParams.processingMode = e.target.value;
        
        // Show/hide S-curve controls and canvas based on mode
        const controlsGrid = document.querySelector('.controls-grid');
        const curveCanvasWrapper = document.querySelector('.curve-canvas-wrapper');
        if (e.target.value === 'stock') {
            controlsGrid.style.display = 'none';
            curveCanvasWrapper.style.display = 'none';
        } else {
            controlsGrid.style.display = 'grid';
            curveCanvasWrapper.style.display = 'block';
        }
        
        updatePreview();
    });
});

// Discard image and return to upload
document.getElementById('discardImage').addEventListener('click', () => {
    currentImageFile = null;
    currentImageCanvas = null;
    originalImageData = null;
    document.getElementById('fileInput').value = '';
    document.getElementById('fileName').textContent = '';
    // Section is already visible, just showing upload area
    document.getElementById('uploadArea').style.display = 'block';
    document.getElementById('previewArea').style.display = 'none';
    document.getElementById('uploadStatus').textContent = '';
    document.getElementById('uploadStatus').className = '';
});

// Reset to defaults
document.getElementById('resetParams').addEventListener('click', () => {
    currentParams = {
        strength: 0.9,
        shadowBoost: 0.0,
        highlightCompress: 1.7,
        midpoint: 0.5,
        saturation: 1.2,
        colorMethod: 'rgb',
        renderMeasured: true,
        processingMode: 'enhanced'
    };
    
    document.getElementById('scurveStrength').value = 0.9;
    document.getElementById('strengthValue').textContent = '0.9';
    document.getElementById('scurveShadow').value = 0.0;
    document.getElementById('shadowValue').textContent = '0.0';
    document.getElementById('scurveHighlight').value = 1.7;
    document.getElementById('highlightValue').textContent = '1.7';
    document.getElementById('scurveMidpoint').value = 0.5;
    document.getElementById('midpointValue').textContent = '0.5';
    document.getElementById('saturation').value = 1.2;
    document.getElementById('saturationValue').textContent = '1.2';
    document.querySelector('input[name="colorMethod"][value="rgb"]').checked = true;
    document.querySelector('input[name="processingMode"][value="enhanced"]').checked = true;
    document.querySelector('.controls-grid').style.display = 'grid';
    document.querySelector('.curve-canvas-wrapper').style.display = 'block';
    
    updatePreview();
});

// Upload processed image
document.getElementById('uploadProcessed').addEventListener('click', async () => {
    if (!currentImageFile || !currentImageCanvas) {
        alert('No image loaded');
        return;
    }
    
    const statusDiv = document.getElementById('uploadStatus');
    const uploadProgress = document.getElementById('uploadProgress');
    const buttonGroup = document.querySelector('.button-group');
    const controlsGrid = document.querySelector('.controls-grid');
    
    // Hide buttons and controls, show progress
    buttonGroup.style.display = 'none';
    controlsGrid.style.display = 'none';
    uploadProgress.style.display = 'block';
    statusDiv.textContent = '';
    statusDiv.className = '';
    
    // Wait for UI to update
    await new Promise(resolve => setTimeout(resolve, 0));
    
    let uploadSucceeded = false;
    
    try {
        let imageBlob;
        
        if (currentParams.processingMode === 'stock') {
            // Stock mode: send raw scaled/cropped image (no processing)
            imageBlob = await resizeImage(currentImageFile, 800, 480, 0.90);
        } else {
            // Enhanced mode: apply S-curve and saturation, but NO dithering
            // Create a temporary canvas for processing
            const tempCanvas = document.createElement('canvas');
            tempCanvas.width = originalImageData.width;
            tempCanvas.height = originalImageData.height;
            const tempCtx = tempCanvas.getContext('2d', { willReadFrequently: true });
            
            // Copy original image data
            const imageDataCopy = tempCtx.createImageData(originalImageData.width, originalImageData.height);
            imageDataCopy.data.set(originalImageData.data);
            
            // Apply S-curve and saturation (imported from image-processor.js)
            if (currentParams.saturation !== 1.0) {
                applySaturation(imageDataCopy, currentParams.saturation);
            }
            
            applyScurveTonemap(
                imageDataCopy,
                currentParams.strength,
                currentParams.shadowBoost,
                currentParams.highlightCompress,
                currentParams.midpoint
            );
            
            // Put processed data to canvas (NO dithering)
            tempCtx.putImageData(imageDataCopy, 0, 0);
            
            // Convert to blob
            imageBlob = await new Promise(resolve => {
                tempCanvas.toBlob(resolve, 'image/jpeg', 0.90);
            });
        }
        
        // Create thumbnail (200x120 or 120x200) from original
        const thumbnailBlob = await resizeImage(currentImageFile, 200, 120, 0.85);
        
        const formData = new FormData();
        formData.append('image', imageBlob, currentImageFile.name);
        formData.append('thumbnail', thumbnailBlob, 'thumb_' + currentImageFile.name);
        formData.append('processingMode', currentParams.processingMode);
        
        const response = await fetch(`${API_BASE}/api/upload`, {
            method: 'POST',
            body: formData
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            uploadSucceeded = true;
            statusDiv.className = 'status-success';
            statusDiv.textContent = `Successfully uploaded: ${data.filename}`;
            
            // Reset state and show upload area again
            currentImageFile = null;
            currentImageCanvas = null;
            originalImageData = null;
            document.getElementById('fileInput').value = '';
            document.getElementById('fileName').textContent = '';
            document.getElementById('uploadArea').style.display = 'block';
            document.getElementById('previewArea').style.display = 'none';
            uploadProgress.style.display = 'none';
            
            // Refresh image gallery
            loadImages();
        } else {
            statusDiv.className = 'status-error';
            statusDiv.textContent = 'Upload failed';
        }
    } catch (error) {
        console.error('Error uploading:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error uploading file';
    } finally {
        // Only show controls if upload failed (still on preview)
        if (!uploadSucceeded) {
            buttonGroup.style.display = 'flex';
            controlsGrid.style.display = 'grid';
            uploadProgress.style.display = 'none';
        }
    }
});

async function loadConfig() {
    try {
        const response = await fetch(`${API_BASE}/api/config`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            console.log('Config API not available (standalone mode)');
            return;
        }
        const data = await response.json();
        
        document.getElementById('autoRotate').checked = data.auto_rotate || false;
        document.getElementById('rotateInterval').value = data.rotate_interval || 3600;
    } catch (error) {
        // Silently fail if API not available (standalone mode)
        console.log('Config API not available (standalone mode)');
    }
}

document.getElementById('configForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    const statusDiv = document.getElementById('configStatus');
    const autoRotate = document.getElementById('autoRotate').checked;
    const rotateInterval = parseInt(document.getElementById('rotateInterval').value);
    
    try {
        const response = await fetch(`${API_BASE}/api/config`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                auto_rotate: autoRotate,
                rotate_interval: rotateInterval
            })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            statusDiv.className = 'status-success';
            statusDiv.textContent = 'Settings saved successfully';
        } else {
            statusDiv.className = 'status-error';
            statusDiv.textContent = 'Failed to save settings';
        }
    } catch (error) {
        console.error('Error saving config:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error saving settings';
    }
});

// Initial load
loadImages();
loadConfig();
loadBatteryStatus();

// Periodic updates - only when page is visible/focused
let imageInterval = null;
let batteryInterval = null;

function startPeriodicUpdates() {
    // Only start if not already running
    if (!imageInterval) {
        imageInterval = setInterval(loadImages, 30000);
    }
    if (!batteryInterval) {
        batteryInterval = setInterval(loadBatteryStatus, 60000);
    }
}

function stopPeriodicUpdates() {
    if (imageInterval) {
        clearInterval(imageInterval);
        imageInterval = null;
    }
    if (batteryInterval) {
        clearInterval(batteryInterval);
        batteryInterval = null;
    }
}

// Listen for page visibility changes
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        console.log('Page hidden - stopping periodic updates');
        stopPeriodicUpdates();
    } else {
        console.log('Page visible - starting periodic updates');
        // Refresh data immediately when page becomes visible
        loadImages();
        loadBatteryStatus();
        startPeriodicUpdates();
    }
});

// Start periodic updates if page is currently visible
if (!document.hidden) {
    startPeriodicUpdates();
}
