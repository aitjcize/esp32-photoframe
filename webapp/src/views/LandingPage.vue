<script setup>
import { ref, onMounted, watch, computed } from "vue";
import { getPreset, getPresetOptions, getDefaultParams } from "@aitjcize/epaper-image-convert";
import { useSettingsStore } from "../stores";
import ImageProcessing from "../components/ImageProcessing.vue";
import ProcessingControls from "../components/ProcessingControls.vue";
import boardsData from "../../../boards/boards.json";

const settingsStore = useSettingsStore();
// Default demo to landscape — the taipei101 sample image is landscape
settingsStore.deviceSettings.displayOrientation = "landscape";

// State
const stableVersion = ref("—");
const devVersion = ref("loading…");
const selectedVersion = ref("stable");
// Whether the selected board actually has a stable build deployed (its stable
// manifest exists). New boards released between tags may only have a dev build.
const stableAvailable = ref(true);
const selectedBoard = ref("waveshare_photopainter_73");
const baseUrl = import.meta.env.BASE_URL;

const supportedBoards = boardsData.map((b) => ({
  ...b,
  value: b.id,
  resolution: `${b.resolution[0]} × ${b.resolution[1]}`,
}));

const selectedBoardMeta = computed(
  () => supportedBoards.find((b) => b.value === selectedBoard.value) || supportedBoards[0]
);

const ecosystem = [
  {
    title: "Firmware",
    blurb: "ESP-IDF firmware for the photoframe. Image pipeline, REST API, Home Assistant.",
    href: "https://github.com/aitjcize/esp32-photoframe",
    tag: "C / ESP-IDF",
  },
  {
    title: "Image Server",
    blurb: "Self-hosted album server. Stores, schedules, and serves processed photos.",
    href: "https://github.com/aitjcize/esp32-photoframe-server",
    tag: "Python",
  },
  {
    title: "Home Assistant",
    blurb: "First-class HA integration. Camera entity, services, automations.",
    href: "https://github.com/aitjcize/ha-esp32-photoframe",
    tag: "HACS",
  },
  {
    title: "Companion App",
    blurb: "iOS and Android app for discovery, upload, AI generation, and settings.",
    href: "https://github.com/aitjcize/esp32-photoframe-app",
    tag: "Flutter",
  },
];

const features = [
  {
    label: "Measured palette",
    body: "Dither against the panel's actual color response, measured from real hardware. No more washed-out greys or muddy reds.",
    figure: "01",
  },
  {
    label: "EPDGZ format",
    body: "A 4-bit-per-pixel pre-processed format. Fraction of the file size, instant render on the device — no on-board image work.",
    figure: "02",
  },
  {
    label: "Deep sleep",
    body: "Weeks of battery on a single charge with auto light-sleep, or always-on for a Home Assistant ambient display.",
    figure: "03",
  },
  {
    label: "REST + Web UI",
    body: "Programmatic control, drag-and-drop uploads, live status, OTA. Everything the stock firmware should have shipped with.",
    figure: "04",
  },
];

const appFeatures = [
  { icon: "mdi-access-point-network", text: "Auto discovery" },
  { icon: "mdi-image-multiple", text: "Gallery management" },
  { icon: "mdi-auto-fix", text: "AI generation" },
  { icon: "mdi-wifi", text: "WiFi setup" },
  { icon: "mdi-cog", text: "Device settings" },
  { icon: "mdi-update", text: "OTA updates" },
  { icon: "mdi-camera", text: "Camera upload" },
  { icon: "mdi-palette", text: "Image processing" },
];

// Image state
const fileInput = ref(null);
const selectedFile = ref(null);

// Processing parameters — start from balanced, but use Stucki dithering (matches the
// hero render). The preset chip will read "custom" since balanced ships with
// floyd-steinberg; that's accurate.
const currentPreset = ref("balanced");
const params = ref({ ...getDefaultParams(), ditherAlgorithm: "stucki" });

const presetNames = getPresetOptions().map((p) => p.value);
const presetKeys = [
  "exposure",
  "saturation",
  "toneMode",
  "contrast",
  "strength",
  "shadowBoost",
  "highlightCompress",
  "midpoint",
  "colorMethod",
  "ditherAlgorithm",
  "compressDynamicRange",
];

function matchesPreset(presetName) {
  const target = getPreset(presetName);
  if (!target) return false;
  const current = params.value;
  for (const key of presetKeys) {
    if (key in target && current[key] !== target[key]) return false;
  }
  return true;
}

function derivePresetFromParams() {
  for (const name of presetNames) {
    if (matchesPreset(name)) return name;
  }
  return "custom";
}

watch(
  params,
  () => {
    currentPreset.value = derivePresetFromParams();
  },
  { deep: true, immediate: true }
);

onMounted(async () => {
  loadVersionInfo();
  loadSampleImage();

  const hash = window.location.hash.slice(1);
  if (hash) {
    const scrollToHash = () => {
      const el = document.getElementById(hash);
      if (el) el.scrollIntoView({ behavior: "smooth", block: "start" });
    };
    // Scroll once after Vue renders (images may not have loaded yet — element is
    // present but layout below it will shift as the hero/quality/app images
    // arrive, leaving the user above the target).
    setTimeout(scrollToHash, 50);
    // Re-scroll once everything loads so we land at the final position.
    if (document.readyState === "complete") {
      setTimeout(scrollToHash, 200);
    } else {
      window.addEventListener("load", () => setTimeout(scrollToHash, 100), { once: true });
    }
  }
});

watch(selectedBoard, () => loadVersionInfo());

async function loadVersionInfo() {
  // The stable version label MUST come from the deployed manifest — that is
  // the exact firmware the flasher installs. The GitHub releases API can be
  // ahead of the deployed site (a release published before the Pages content
  // refreshes), which made the page advertise a version it wasn't actually
  // serving (#108). Fall back to the API only when no stable manifest is
  // deployed for this board.
  let manifestVersion = null;
  try {
    const stableManifest = await fetch(baseUrl + selectedBoard.value + "/manifest.json");
    stableAvailable.value = stableManifest.ok;
    if (stableManifest.ok) {
      manifestVersion = (await stableManifest.json()).version || null;
    }
  } catch {
    stableAvailable.value = false;
  }
  if (!stableAvailable.value && selectedVersion.value === "stable") {
    selectedVersion.value = "dev";
  }

  if (manifestVersion) {
    stableVersion.value = manifestVersion;
  } else {
    try {
      const stableResponse = await fetch(
        "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
      );
      stableVersion.value = (await stableResponse.json()).tag_name;
    } catch (error) {
      console.error("Error fetching stable version:", error);
      stableVersion.value = "unknown";
    }
  }

  try {
    let devResponse = await fetch(baseUrl + selectedBoard.value + "/manifest-dev.json");
    if (!devResponse.ok) {
      devResponse = await fetch(baseUrl + "manifest-dev.json");
    }
    if (devResponse.ok) {
      const devData = await devResponse.json();
      devVersion.value = devData.version || "dev";
    }
  } catch (error) {
    console.error("Error fetching dev version:", error);
  }
}

async function loadSampleImage() {
  try {
    const response = await fetch(baseUrl + "taipei101.jpg");
    if (!response.ok) return;
    const blob = await response.blob();
    selectedFile.value = new File([blob], "taipei101.jpg", { type: "image/jpeg" });
  } catch (error) {
    console.log("Sample image not available:", error.message);
  }
}

function triggerFileSelect() {
  fileInput.value?.click();
}
async function onFileSelected(event) {
  const file = event.target.files?.[0];
  if (file) selectedFile.value = file;
}
async function onDrop(event) {
  const file = event.dataTransfer?.files?.[0];
  if (file && file.type.match("image.*")) selectedFile.value = file;
}
function onPresetChange(presetName) {
  if (presetName === "custom") return;
  const preset = getPreset(presetName);
  if (preset) {
    // eslint-disable-next-line no-unused-vars
    const { name, title, description, ...processingParams } = preset;
    Object.assign(params.value, processingParams);
    currentPreset.value = presetName;
  }
}
function onParamsUpdate(newParams) {
  Object.assign(params.value, newParams);
}
function newImage() {
  selectedFile.value = null;
  if (fileInput.value) fileInput.value.value = "";
}

function scrollTo(id) {
  const el = document.getElementById(id);
  if (el) {
    history.replaceState(null, "", `#${id}`);
    el.scrollIntoView({ behavior: "smooth", block: "start" });
  }
}
</script>

<template>
  <v-app>
    <div class="page">
      <!-- ─────────────────────────────────────────  NAV  ───────────────────────────────────────── -->
      <header class="nav">
        <a class="nav-brand" href="#" @click.prevent="scrollTo('top')">
          <img :src="`${baseUrl}icon.svg`" alt="" class="nav-mark" />
          <span class="nav-wordmark">esp32<span class="nav-hyphen">-</span>photoframe</span>
        </a>
        <nav class="nav-links">
          <a @click.prevent="scrollTo('quality')">Quality</a>
          <a @click.prevent="scrollTo('demo')">Demo</a>
          <a @click.prevent="scrollTo('hardware')">Hardware</a>
          <a @click.prevent="scrollTo('flash')">Flash</a>
          <a @click.prevent="scrollTo('companion-app')">App</a>
        </nav>
        <div class="nav-meta">
          <span class="version-chip">{{ stableVersion }}</span>
          <a
            class="nav-github"
            href="https://github.com/aitjcize/esp32-photoframe"
            target="_blank"
            rel="noopener"
            aria-label="View on GitHub"
          >
            <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
              <path
                d="M12 .5C5.65.5.5 5.65.5 12c0 5.08 3.29 9.39 7.86 10.91.58.1.79-.25.79-.56v-2c-3.2.7-3.88-1.36-3.88-1.36-.52-1.34-1.28-1.69-1.28-1.69-1.05-.71.08-.7.08-.7 1.16.08 1.77 1.19 1.77 1.19 1.03 1.77 2.7 1.26 3.36.96.1-.75.4-1.26.73-1.55-2.55-.29-5.24-1.28-5.24-5.7 0-1.26.45-2.29 1.18-3.1-.12-.29-.51-1.46.11-3.04 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.79 0c2.2-1.49 3.18-1.18 3.18-1.18.62 1.58.23 2.75.11 3.04.74.81 1.18 1.84 1.18 3.1 0 4.43-2.69 5.4-5.26 5.69.41.36.78 1.06.78 2.14v3.17c0 .31.21.67.8.56C20.21 21.39 23.5 17.08 23.5 12 23.5 5.65 18.35.5 12 .5Z"
              />
            </svg>
          </a>
        </div>
      </header>

      <span id="top" class="anchor"></span>

      <!-- ─────────────────────────────────────────  HERO  ───────────────────────────────────────── -->
      <section class="hero">
        <div class="hero-grid">
          <div class="hero-text">
            <div class="eyebrow">
              Open firmware for ESP32 e-paper photo frames · {{ stableVersion }}
            </div>
            <h1 class="display">
              Picture-book<br />
              <em class="wonk">quality</em> on<br />
              electronic paper.
            </h1>
            <p class="dek">
              <strong>esp32-photoframe</strong> is an open-source replacement firmware for
              off-the-shelf ESP32 e-paper photo frames (<em>Waveshare PhotoPainter</em>,
              <em>Seeed Studio XIAO</em> and <em>reTerminal</em>). It ships with measured-palette
              dithering, a proper REST API, AI image generation, Home Assistant integration, and
              weeks of battery on deep sleep — for an always-on ambient display you'd actually want
              to look at.
            </p>
            <div class="cta-row">
              <button class="btn btn-primary" @click="scrollTo('demo')">
                Try the demo
                <span class="btn-arrow" aria-hidden="true">→</span>
              </button>
              <button class="btn btn-ghost" @click="scrollTo('flash')">Flash my frame</button>
            </div>
            <div class="hero-meta">
              <span><strong>5</strong> boards</span>
              <span class="dot">·</span>
              <span><strong>6-color</strong> e-paper</span>
              <span class="dot">·</span>
              <span><strong>MIT</strong> license</span>
            </div>
          </div>
          <div class="hero-visual">
            <div class="frame-card">
              <span class="frame-pp">the frame</span>
              <div class="frame-image">
                <img
                  :src="`${baseUrl}taipei101_dithered.png`"
                  alt="Taipei 101 dithered for e-paper"
                />
              </div>
              <div class="frame-caption">
                <span>Taipei 101 at dusk</span>
                <span class="frame-meta">Stucki · measured palette</span>
              </div>
            </div>
          </div>
        </div>
      </section>

      <!-- ───────────────────────────────────  QUALITY SHOWCASE  ─────────────────────────────────── -->
      <section id="quality" class="section section-quality">
        <div class="section-head">
          <span class="section-kicker">Image quality</span>
        </div>
        <h2 class="section-title">Stock dithering, then <em class="wonk">measured</em> palette.</h2>
        <p class="section-lede">
          Most e-paper firmware dithers against the manufacturer's idealized palette. We measure the
          actual colors the panel renders, then dither against those — so reds stay red and midtones
          don't go muddy. All three captures are shot off the same panel.
        </p>

        <div class="compare">
          <figure class="compare-side">
            <div class="compare-img">
              <img :src="`${baseUrl}sample.jpg`" alt="Original source photograph" />
            </div>
            <figcaption>
              <span class="compare-label">Original</span>
              <span class="compare-meta">Source photograph</span>
            </figcaption>
          </figure>
          <figure class="compare-side">
            <div class="compare-img">
              <img :src="`${baseUrl}stock_algorithm.bmp`" alt="Stock firmware output on device" />
            </div>
            <figcaption>
              <span class="compare-label">Stock</span>
              <span class="compare-meta">Manufacturer's idealized palette · on device</span>
            </figcaption>
          </figure>
          <figure class="compare-side compare-ours">
            <div class="compare-img">
              <img :src="`${baseUrl}our_algorithm.png`" alt="Our firmware output on device" />
            </div>
            <figcaption>
              <span class="compare-label">Ours</span>
              <span class="compare-meta">Measured palette · S-curve tone · on device</span>
            </figcaption>
          </figure>
        </div>
      </section>

      <!-- ─────────────────────────────────────  INTERACTIVE DEMO  ──────────────────────────────── -->
      <section id="demo" class="section section-demo">
        <div class="section-head">
          <span class="section-kicker">Interactive demo</span>
        </div>
        <h2 class="section-title">Drop in <em class="wonk">your</em> own photo.</h2>
        <p class="section-lede">
          Drag the slider to compare the original against our pipeline. Adjust exposure, contrast,
          dithering, palette — every setting that ships in the firmware.
        </p>

        <input
          ref="fileInput"
          type="file"
          accept="image/*"
          style="display: none"
          @change="onFileSelected"
        />

        <div class="demo-card">
          <div class="demo-card-head">
            <span class="demo-card-label">Original vs processed</span>
            <button v-if="selectedFile" class="link-btn" @click="newImage">↻ New image</button>
          </div>
          <div class="demo-card-body">
            <div
              v-if="!selectedFile"
              class="upload"
              @click="triggerFileSelect"
              @dragover.prevent
              @drop.prevent="onDrop"
            >
              <svg
                width="42"
                height="42"
                viewBox="0 0 24 24"
                fill="none"
                stroke="currentColor"
                stroke-width="1.5"
                aria-hidden="true"
              >
                <path
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  d="M3 16.5v2.25A2.25 2.25 0 0 0 5.25 21h13.5A2.25 2.25 0 0 0 21 18.75V16.5M16.5 12 12 16.5m0 0L7.5 12m4.5 4.5V3"
                />
              </svg>
              <p class="upload-title">Click or drag an image</p>
              <p class="upload-meta">JPG · PNG · WebP</p>
            </div>
            <ImageProcessing v-else :image-file="selectedFile" :params="params" />
          </div>
        </div>

        <div class="demo-card">
          <div class="demo-card-head">
            <span class="demo-card-label">Processing parameters</span>
            <span class="demo-card-meta">{{ currentPreset }}</span>
          </div>
          <div class="demo-card-body">
            <ProcessingControls
              :params="params"
              :preset="currentPreset"
              @update:params="onParamsUpdate"
              @preset-change="onPresetChange"
            />
          </div>
        </div>
      </section>

      <!-- ─────────────────────────────────────────  FEATURES  ──────────────────────────────────── -->
      <section class="section section-features">
        <div class="section-head">
          <span class="section-kicker">Why it's different</span>
        </div>
        <h2 class="section-title">Built for photographs, <em class="wonk">not</em> spec sheets.</h2>

        <div class="feature-grid">
          <article v-for="f in features" :key="f.figure" class="feature">
            <span class="feature-num">{{ f.figure }}</span>
            <h3 class="feature-label">{{ f.label }}</h3>
            <p class="feature-body">{{ f.body }}</p>
          </article>
        </div>
      </section>

      <!-- ─────────────────────────────────────────  HARDWARE  ──────────────────────────────────── -->
      <section id="hardware" class="section section-hardware">
        <div class="section-head">
          <span class="section-kicker">Supported hardware</span>
        </div>
        <h2 class="section-title">Off-the-shelf boards, <em class="wonk">opened</em> up.</h2>
        <p class="section-lede">
          Bring your own hardware. Four ESP32-S3 photoframes are supported today, with shared image
          pipeline and configuration.
        </p>

        <div class="board-grid">
          <a
            v-for="b in supportedBoards"
            :key="b.value"
            :href="b.url"
            target="_blank"
            rel="noopener"
            class="board"
          >
            <div class="board-display">{{ b.display }}</div>
            <div class="board-name">{{ b.label }}</div>
            <div class="board-meta">
              <span>{{ b.resolution }} px</span>
              <span class="dot">·</span>
              <span>{{ b.storage }}</span>
            </div>
            <span class="board-arrow" aria-hidden="true">↗</span>
          </a>
        </div>
      </section>

      <!-- ──────────────────────────────────────────  FLASH  ────────────────────────────────────── -->
      <section id="flash" class="section section-flash">
        <div class="section-head">
          <span class="section-kicker">Flash your frame</span>
        </div>
        <h2 class="section-title">One USB-C cable. <em class="wonk">No</em> toolchain.</h2>
        <p class="section-lede">
          The web flasher uses Web Serial. Plug in a supported board, pick a release, install. Stock
          firmware is replaced; your photos and settings survive on storage.
        </p>

        <div class="flash-grid">
          <div class="flash-controls">
            <div class="flash-row">
              <label class="flash-label">Release</label>
              <div class="radio-row">
                <label class="radio" :class="{ 'radio-disabled': !stableAvailable }">
                  <input
                    v-model="selectedVersion"
                    type="radio"
                    value="stable"
                    :disabled="!stableAvailable"
                  />
                  <span class="radio-dot"></span>
                  <span class="radio-text">
                    <strong>Stable</strong>
                    <em class="radio-tag">{{
                      stableAvailable ? stableVersion : "none for this board yet"
                    }}</em>
                  </span>
                </label>
                <label class="radio">
                  <input v-model="selectedVersion" type="radio" value="dev" />
                  <span class="radio-dot"></span>
                  <span class="radio-text">
                    <strong>Dev</strong>
                    <em class="radio-tag">{{ devVersion }}</em>
                  </span>
                </label>
              </div>
            </div>

            <div class="flash-row">
              <label class="flash-label" for="flash-board">Board</label>
              <select id="flash-board" v-model="selectedBoard" class="select">
                <option v-for="b in supportedBoards" :key="b.value" :value="b.value">
                  {{ b.label }} — {{ b.display }}
                </option>
              </select>
            </div>

            <div class="flash-row flash-action">
              <esp-web-install-button
                :key="selectedBoard + selectedVersion"
                :manifest="
                  (baseUrl.endsWith('/') ? baseUrl : baseUrl + '/') +
                  selectedBoard +
                  '/' +
                  (selectedVersion === 'stable' ? 'manifest.json' : 'manifest-dev.json')
                "
              >
                <button slot="activate" class="btn btn-primary btn-large">
                  ⚡ Install firmware
                  <span class="btn-arrow" aria-hidden="true">→</span>
                </button>
              </esp-web-install-button>
              <p class="flash-note">
                Requires Chrome, Edge, or Opera. Web Serial is not available in Safari or Firefox.
              </p>
            </div>
          </div>

          <aside class="flash-aside">
            <h4>How it goes</h4>
            <ol class="flash-steps">
              <li>Connect the board over USB-C.</li>
              <li>Click <em>Install firmware</em> and select the serial port.</li>
              <li>Wait for the flash to complete (about a minute).</li>
              <li>The device restarts and starts a <code>PhotoFrame-XXXX</code> WiFi AP.</li>
              <li>Connect, configure WiFi, you're on.</li>
            </ol>
            <div class="flash-board-meta">
              <span class="flash-board-name">{{ selectedBoardMeta.label }}</span>
              <span class="flash-board-spec">
                {{ selectedBoardMeta.display }} · {{ selectedBoardMeta.resolution }}
              </span>
            </div>
          </aside>
        </div>
      </section>

      <!-- ────────────────────────────────────────  COMPANION  ──────────────────────────────────── -->
      <section id="companion-app" class="section section-app">
        <div class="app-canvas">
          <img
            class="app-graphic"
            :src="`${baseUrl}feature_graphic.png`"
            alt="ESP Frame companion app"
          />
          <div class="app-text">
            <div class="section-head section-head-light">
              <span class="section-kicker">Companion app</span>
            </div>
            <h2 class="section-title section-title-light">
              The frame, in <em class="wonk">your</em> pocket.
            </h2>
            <p class="section-lede section-lede-light">
              Discover devices on your network, browse galleries, generate AI images, and manage
              settings — all without opening a browser. In public testing now.
            </p>
            <div class="app-stores">
              <a
                href="https://apps.apple.com/tw/app/esp-frame/id6762510995?l=en-GB"
                target="_blank"
                rel="noopener"
                class="store"
              >
                <v-icon icon="mdi-apple" size="28" />
                <div>
                  <span class="store-os">App Store</span>
                  <span class="store-track">USD 2.99</span>
                </div>
              </a>
              <a
                href="https://play.google.com/store/apps/details?id=com.aitjcize.espframe"
                target="_blank"
                rel="noopener"
                class="store"
              >
                <v-icon icon="mdi-google-play" size="28" />
                <div>
                  <span class="store-os">Google Play</span>
                  <span class="store-track">Free</span>
                </div>
              </a>
            </div>
            <div class="app-features">
              <span v-for="f in appFeatures" :key="f.text" class="app-feature">
                <v-icon :icon="f.icon" size="16" />
                {{ f.text }}
              </span>
            </div>
          </div>
        </div>
      </section>

      <!-- ─────────────────────────────────────────  ECOSYSTEM  ─────────────────────────────────── -->
      <section class="section section-eco">
        <div class="section-head">
          <span class="section-kicker">The whole stack</span>
        </div>
        <h2 class="section-title">Four repos, <em class="wonk">one</em> photoframe.</h2>
        <p class="section-lede">
          Use just the firmware, or add the server, the Home Assistant integration, and the app for
          the full experience. All MIT, all open.
        </p>

        <div class="eco-grid">
          <a
            v-for="e in ecosystem"
            :key="e.title"
            :href="e.href"
            target="_blank"
            rel="noopener"
            class="eco"
          >
            <span class="eco-tag">{{ e.tag }}</span>
            <h3 class="eco-title">{{ e.title }}</h3>
            <p class="eco-blurb">{{ e.blurb }}</p>
            <span class="eco-arrow" aria-hidden="true">↗ View on GitHub</span>
          </a>
        </div>
      </section>

      <!-- ──────────────────────────────────────────  FOOTER  ───────────────────────────────────── -->
      <footer class="footer">
        <div class="footer-grid">
          <div class="footer-brand">
            <img :src="`${baseUrl}icon.svg`" alt="" class="footer-mark" />
            <div>
              <div class="footer-wordmark">esp32-photoframe</div>
              <div class="footer-tag">An open e-paper photoframe firmware.</div>
            </div>
          </div>
          <div class="footer-links">
            <a href="https://github.com/aitjcize/esp32-photoframe" target="_blank" rel="noopener"
              >Firmware</a
            >
            <a
              href="https://github.com/aitjcize/esp32-photoframe-server"
              target="_blank"
              rel="noopener"
              >Server</a
            >
            <a href="https://github.com/aitjcize/ha-esp32-photoframe" target="_blank" rel="noopener"
              >Home Assistant</a
            >
            <a
              href="https://github.com/aitjcize/esp32-photoframe-app"
              target="_blank"
              rel="noopener"
              >App</a
            >
            <a
              href="https://github.com/aitjcize/esp32-photoframe/blob/main/LICENSE"
              target="_blank"
              rel="noopener"
              >License</a
            >
          </div>
        </div>
        <div class="footer-rule"></div>
        <div class="footer-bottom">
          <span>© {{ new Date().getFullYear() }} aitjcize. MIT licensed.</span>
          <span>Made with patience, for paper.</span>
        </div>
      </footer>
    </div>
  </v-app>
</template>

<style scoped>
/* ───────────────────────────────────  TOKENS  ─────────────────────────────────── */
.page {
  --paper: #f5efe2;
  --paper-deep: #ebe1cc;
  --paper-soft: #faf6ec;
  --ink: #2b1d0f;
  --ink-2: #4a3520;
  --ink-3: #7a6346;
  --ink-4: #a89478;
  --rule: #d6c8ad;
  --accent: #c98a3b;
  --accent-deep: #a06a26;
  --accent-soft: #e8c89a;
  --espresso: #1f140a;
  --espresso-2: #2c1d10;
  --cream: #f4ead6;

  --serif: "Fraunces", "Iowan Old Style", Georgia, serif;
  --sans: "Manrope", -apple-system, BlinkMacSystemFont, "Helvetica Neue", sans-serif;

  background: var(--paper);
  color: var(--ink);
  font-family: var(--sans);
  font-feature-settings: "ss01", "ss02";
  -webkit-font-smoothing: antialiased;
  min-height: 100vh;
  position: relative;
  overflow-x: hidden;
}

/* Subtle paper grain across the whole page */
.page::before {
  content: "";
  position: fixed;
  inset: 0;
  pointer-events: none;
  opacity: 0.5;
  mix-blend-mode: multiply;
  z-index: 0;
  background-image: url("data:image/svg+xml;utf8,<svg viewBox='0 0 200 200' xmlns='http://www.w3.org/2000/svg'><filter id='n'><feTurbulence type='fractalNoise' baseFrequency='0.85' numOctaves='2' stitchTiles='stitch'/><feColorMatrix values='0 0 0 0 0.17 0 0 0 0 0.11 0 0 0 0 0.06 0 0 0 0.06 0'/></filter><rect width='100%' height='100%' filter='url(%23n)'/></svg>");
}

/* Anchor offset for sticky nav */
.anchor {
  display: block;
  height: 0;
  visibility: hidden;
}

/* ───────────────────────────────────  NAV  ─────────────────────────────────── */
.nav {
  position: sticky;
  top: 0;
  z-index: 50;
  display: grid;
  grid-template-columns: auto 1fr auto;
  align-items: center;
  gap: 2rem;
  padding: 0.9rem clamp(1.25rem, 4vw, 3rem);
  background: color-mix(in srgb, var(--paper) 88%, transparent);
  backdrop-filter: saturate(160%) blur(14px);
  -webkit-backdrop-filter: saturate(160%) blur(14px);
  border-bottom: 1px solid var(--rule);
}
.nav-brand {
  display: inline-flex;
  align-items: center;
  gap: 0.65rem;
  text-decoration: none;
  color: var(--ink);
}
.nav-mark {
  width: 28px;
  height: 28px;
  border-radius: 6px;
}
.nav-wordmark {
  font-family: var(--serif);
  font-weight: 600;
  font-size: 1.05rem;
  letter-spacing: -0.01em;
  font-variation-settings:
    "opsz" 14,
    "SOFT" 30;
}
.nav-hyphen {
  color: var(--accent);
  margin: 0 0.04em;
}
.nav-links {
  display: flex;
  justify-content: center;
  gap: 1.6rem;
}
.nav-links a {
  font-size: 0.78rem;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: var(--ink-3);
  text-decoration: none;
  cursor: pointer;
  transition: color 0.2s;
}
.nav-links a:hover {
  color: var(--ink);
}
.nav-meta {
  display: inline-flex;
  align-items: center;
  gap: 1rem;
}
.version-chip {
  font-size: 0.72rem;
  letter-spacing: 0.1em;
  padding: 0.35rem 0.7rem;
  border: 1px solid var(--rule);
  border-radius: 999px;
  color: var(--ink-2);
  background: var(--paper-soft);
}
.nav-github {
  color: var(--ink-2);
  display: inline-flex;
  align-items: center;
  transition: color 0.2s;
}
.nav-github:hover {
  color: var(--ink);
}
@media (max-width: 880px) {
  .nav-links {
    display: none;
  }
  .nav {
    grid-template-columns: auto auto;
    gap: 1rem;
  }
  .nav-meta {
    justify-self: end;
  }
}

/* ───────────────────────────────────  TYPE  ─────────────────────────────────── */
.eyebrow,
.section-kicker {
  font-family: var(--sans);
  font-size: 0.72rem;
  letter-spacing: 0.22em;
  text-transform: uppercase;
  color: var(--ink-3);
  font-weight: 500;
}

.display {
  font-family: var(--serif);
  font-weight: 360;
  font-size: clamp(2.6rem, 7.5vw, 6rem);
  line-height: 0.98;
  letter-spacing: -0.025em;
  color: var(--ink);
  margin: 1.5rem 0 1.75rem;
  font-variation-settings:
    "opsz" 144,
    "SOFT" 40,
    "WONK" 0;
  text-wrap: balance;
}

.section-title {
  font-family: var(--serif);
  font-weight: 380;
  font-size: clamp(2rem, 4.6vw, 3.6rem);
  line-height: 1.04;
  letter-spacing: -0.02em;
  color: var(--ink);
  margin: 0.6rem 0 1.4rem;
  font-variation-settings:
    "opsz" 96,
    "SOFT" 30,
    "WONK" 0;
  max-width: 22ch;
  text-wrap: balance;
}

em.wonk {
  font-style: italic;
  color: var(--accent-deep);
  font-variation-settings:
    "opsz" 144,
    "SOFT" 50,
    "WONK" 1;
  font-weight: 380;
}

.dek,
.section-lede {
  font-size: clamp(1.05rem, 1.45vw, 1.2rem);
  line-height: 1.65;
  color: var(--ink-2);
  max-width: 56ch;
  margin: 0 0 2rem;
}
.dek em,
.section-lede em {
  font-family: var(--serif);
  font-style: italic;
  color: var(--ink);
  font-variation-settings:
    "opsz" 14,
    "SOFT" 50;
}
.section-lede {
  margin-bottom: 3.5rem;
}

/* ───────────────────────────────────  HERO  ─────────────────────────────────── */
.hero {
  position: relative;
  z-index: 1;
  padding: clamp(3rem, 7vw, 6rem) clamp(1.25rem, 5vw, 4rem) clamp(4rem, 9vw, 8rem);
  max-width: 1480px;
  margin: 0 auto;
}
.hero-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr);
  gap: clamp(2.5rem, 5vw, 5rem);
  align-items: center;
}
@media (max-width: 960px) {
  .hero-grid {
    grid-template-columns: 1fr;
  }
}

.hero-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 0.6rem 1rem;
  font-size: 0.85rem;
  color: var(--ink-3);
  letter-spacing: 0.02em;
}
.hero-meta strong {
  color: var(--ink);
  font-weight: 600;
  font-feature-settings: "tnum";
}
.dot {
  color: var(--ink-4);
}

.cta-row {
  display: flex;
  flex-wrap: wrap;
  gap: 0.85rem;
  margin: 0 0 2.4rem;
}

.btn {
  display: inline-flex;
  align-items: center;
  gap: 0.55rem;
  padding: 0.95rem 1.5rem;
  font-family: var(--sans);
  font-size: 0.95rem;
  font-weight: 500;
  letter-spacing: 0.01em;
  border-radius: 999px;
  border: 1px solid transparent;
  cursor: pointer;
  transition:
    transform 0.2s ease,
    background 0.25s,
    color 0.25s,
    border-color 0.25s;
}
.btn-primary {
  background: var(--ink);
  color: var(--paper);
}
.btn-primary:hover {
  background: var(--espresso);
  transform: translateY(-1px);
}
.btn-ghost {
  background: transparent;
  color: var(--ink);
  border-color: var(--ink-2);
}
.btn-ghost:hover {
  background: var(--ink);
  color: var(--paper);
}
.btn-large {
  padding: 1.1rem 1.85rem;
  font-size: 1.02rem;
}
.btn-arrow {
  transition: transform 0.2s;
}
.btn:hover .btn-arrow {
  transform: translateX(3px);
}

.hero-visual {
  position: relative;
}
.frame-card {
  background: var(--paper-soft);
  border: 1px solid var(--rule);
  border-radius: 4px;
  padding: 1.4rem 1.4rem 1.6rem;
  position: relative;
  box-shadow:
    0 1px 0 rgba(43, 29, 15, 0.04),
    0 24px 60px -24px rgba(43, 29, 15, 0.22);
  transform: rotate(-1deg);
}
.frame-pp {
  display: block;
  font-size: 0.68rem;
  text-transform: uppercase;
  letter-spacing: 0.22em;
  color: var(--ink-3);
  margin-bottom: 0.9rem;
  font-feature-settings: "tnum";
}
.frame-image {
  background: var(--espresso);
  border-radius: 2px;
  overflow: hidden;
  aspect-ratio: 800 / 480;
  display: flex;
  align-items: center;
  justify-content: center;
}
.frame-image img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  filter: contrast(1.04) saturate(0.97);
}
.frame-caption {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: 1rem;
  margin-top: 0.9rem;
  font-size: 0.82rem;
  color: var(--ink-2);
  font-family: var(--serif);
  font-style: italic;
  font-variation-settings:
    "opsz" 14,
    "SOFT" 50;
}
.frame-meta {
  color: var(--ink-3);
  font-style: normal;
  font-family: var(--sans);
  font-size: 0.72rem;
  letter-spacing: 0.1em;
  text-transform: uppercase;
}

/* ───────────────────────────────────  SECTIONS  ─────────────────────────────────── */
.section {
  position: relative;
  z-index: 1;
  padding: clamp(4rem, 8vw, 8rem) clamp(1.25rem, 5vw, 4rem);
  max-width: 1480px;
  margin: 0 auto;
}

.section-head {
  display: inline-flex;
  align-items: center;
  gap: 0.85rem;
  margin-bottom: 1.1rem;
}
.section-head::before {
  content: "";
  width: 28px;
  height: 1px;
  background: var(--rule);
}
.section + .section {
  border-top: 1px solid var(--rule);
}

/* ───────────────────────────────  QUALITY SHOWCASE  ─────────────────────────────── */
.compare {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: clamp(1rem, 2vw, 1.75rem);
  align-items: start;
  max-width: 1040px;
  margin: 0 auto;
}
@media (max-width: 880px) {
  .compare {
    grid-template-columns: 1fr 1fr;
  }
}
@media (max-width: 560px) {
  .compare {
    grid-template-columns: 1fr;
  }
}
.compare-side {
  margin: 0;
  display: grid;
  gap: 1rem;
}
.compare-side.compare-ours {
  transform: translateY(1.4rem);
}
@media (max-width: 880px) {
  .compare-side.compare-ours {
    transform: none;
  }
}
.compare-img {
  background: var(--paper-deep);
  border-radius: 4px;
  overflow: hidden;
  aspect-ratio: 480 / 800;
  border: 1px solid var(--rule);
}
.compare-img img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  image-rendering: -webkit-optimize-contrast;
}
.compare-side figcaption {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  gap: 0.75rem;
  padding: 0 0.25rem;
}
.compare-label {
  font-family: var(--serif);
  font-size: 1.4rem;
  font-weight: 400;
  font-variation-settings:
    "opsz" 30,
    "SOFT" 30;
}
.compare-side.compare-ours .compare-label {
  font-style: italic;
  color: var(--accent-deep);
  font-variation-settings:
    "opsz" 30,
    "WONK" 1;
}
.compare-meta {
  font-size: 0.75rem;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  color: var(--ink-3);
}

/* ───────────────────────────────  DEMO CARD  ─────────────────────────────── */
.demo-card {
  background: var(--paper-soft);
  border: 1px solid var(--rule);
  border-radius: 6px;
  margin-bottom: 1.5rem;
  overflow: hidden;
}
.demo-card-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 1.05rem 1.5rem;
  border-bottom: 1px solid var(--rule);
  background: linear-gradient(to bottom, var(--paper-soft), var(--paper));
}
.demo-card-label {
  font-size: 0.75rem;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: var(--ink-3);
  font-weight: 500;
}
.demo-card-meta {
  font-family: var(--serif);
  font-style: italic;
  color: var(--accent-deep);
  font-size: 0.95rem;
  font-variation-settings:
    "opsz" 14,
    "WONK" 1;
}
.demo-card-body {
  padding: clamp(1rem, 2.5vw, 2rem);
}
.link-btn {
  background: none;
  border: none;
  cursor: pointer;
  font-family: var(--sans);
  font-size: 0.85rem;
  color: var(--accent-deep);
  font-weight: 500;
}
.link-btn:hover {
  color: var(--accent);
}

.upload {
  border: 1.5px dashed var(--ink-4);
  border-radius: 4px;
  padding: clamp(2.5rem, 7vw, 5rem) 2rem;
  text-align: center;
  cursor: pointer;
  background: var(--paper);
  transition:
    border-color 0.2s,
    background 0.2s;
  color: var(--ink-3);
}
.upload:hover {
  border-color: var(--accent);
  background: var(--paper-soft);
}
.upload-title {
  font-family: var(--serif);
  font-size: 1.4rem;
  margin: 1rem 0 0.3rem;
  color: var(--ink);
  font-variation-settings:
    "opsz" 30,
    "SOFT" 30;
}
.upload-meta {
  font-size: 0.78rem;
  letter-spacing: 0.18em;
  text-transform: uppercase;
}

/* ───────────────────────────────  FEATURES  ─────────────────────────────── */
.feature-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
  gap: 1px;
  background: var(--rule);
  border: 1px solid var(--rule);
  border-radius: 6px;
  overflow: hidden;
}
.feature {
  background: var(--paper-soft);
  padding: clamp(1.5rem, 3vw, 2.5rem);
  position: relative;
  transition: background 0.3s;
}
.feature:hover {
  background: var(--paper);
}
.feature-num {
  font-family: var(--serif);
  font-style: italic;
  font-size: 0.9rem;
  color: var(--accent);
  font-variation-settings:
    "opsz" 14,
    "WONK" 1;
  font-feature-settings: "tnum";
}
.feature-label {
  font-family: var(--serif);
  font-weight: 400;
  font-size: 1.55rem;
  margin: 0.4rem 0 0.7rem;
  letter-spacing: -0.015em;
  color: var(--ink);
  font-variation-settings:
    "opsz" 36,
    "SOFT" 40;
}
.feature-body {
  font-size: 0.97rem;
  line-height: 1.6;
  color: var(--ink-2);
  margin: 0;
}

/* ───────────────────────────────  HARDWARE  ─────────────────────────────── */
.board-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
  gap: 1.25rem;
}
.board {
  position: relative;
  text-decoration: none;
  color: inherit;
  background: var(--paper-soft);
  border: 1px solid var(--rule);
  border-radius: 6px;
  padding: 1.6rem 1.6rem 1.4rem;
  transition:
    transform 0.25s,
    box-shadow 0.25s,
    border-color 0.25s;
  display: block;
}
.board:hover {
  transform: translateY(-3px);
  border-color: var(--accent);
  box-shadow: 0 18px 40px -22px rgba(43, 29, 15, 0.24);
}
.board-display {
  font-family: var(--serif);
  font-size: 2rem;
  font-weight: 360;
  letter-spacing: -0.02em;
  color: var(--accent-deep);
  font-variation-settings:
    "opsz" 60,
    "SOFT" 30,
    "WONK" 1;
  font-style: italic;
}
.board-name {
  font-family: var(--serif);
  font-size: 1.15rem;
  font-weight: 500;
  margin: 0.4rem 0 0.5rem;
  font-variation-settings:
    "opsz" 24,
    "SOFT" 30;
  color: var(--ink);
}
.board-meta {
  font-size: 0.82rem;
  color: var(--ink-3);
  display: flex;
  flex-wrap: wrap;
  gap: 0.4rem 0.7rem;
}
.board-arrow {
  position: absolute;
  top: 1.4rem;
  right: 1.4rem;
  color: var(--ink-3);
  font-size: 1.05rem;
  transition:
    color 0.25s,
    transform 0.25s;
}
.board:hover .board-arrow {
  color: var(--accent);
  transform: translate(2px, -2px);
}

/* ───────────────────────────────  FLASH  ─────────────────────────────── */
.flash-grid {
  display: grid;
  grid-template-columns: minmax(0, 1.4fr) minmax(0, 1fr);
  gap: clamp(1.5rem, 3vw, 3rem);
  align-items: stretch;
}
@media (max-width: 880px) {
  .flash-grid {
    grid-template-columns: 1fr;
  }
}
.flash-controls {
  background: var(--paper-soft);
  border: 1px solid var(--rule);
  border-radius: 6px;
  padding: clamp(1.5rem, 3vw, 2.4rem);
  display: grid;
  gap: 1.6rem;
  align-content: start;
}
.flash-row {
  display: grid;
  gap: 0.85rem;
}
.flash-label {
  font-size: 0.72rem;
  letter-spacing: 0.2em;
  text-transform: uppercase;
  color: var(--ink-3);
  font-weight: 500;
}
.radio-row {
  display: flex;
  flex-wrap: wrap;
  gap: 0.7rem;
}
.radio {
  display: inline-flex;
  align-items: center;
  gap: 0.7rem;
  padding: 0.85rem 1.1rem;
  background: var(--paper);
  border: 1px solid var(--rule);
  border-radius: 999px;
  cursor: pointer;
  transition:
    border-color 0.2s,
    background 0.2s;
}
.radio:hover {
  border-color: var(--ink-3);
}
.radio-disabled {
  opacity: 0.45;
  cursor: not-allowed;
}
.radio-disabled:hover {
  border-color: var(--rule);
}
.radio input {
  display: none;
}
.radio-dot {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  border: 1.5px solid var(--ink-3);
  position: relative;
  flex-shrink: 0;
  transition: border-color 0.2s;
}
.radio input:checked + .radio-dot {
  border-color: var(--accent);
}
.radio input:checked + .radio-dot::after {
  content: "";
  position: absolute;
  inset: 2px;
  background: var(--accent);
  border-radius: 50%;
}
.radio input:checked ~ .radio-text strong {
  color: var(--ink);
}
.radio:has(input:checked) {
  border-color: var(--accent);
  background: var(--paper-soft);
}
.radio-text {
  display: inline-flex;
  align-items: baseline;
  gap: 0.55rem;
}
.radio-text strong {
  font-weight: 500;
  font-size: 0.95rem;
  color: var(--ink-2);
}
.radio-tag {
  font-family: var(--serif);
  font-style: italic;
  color: var(--accent-deep);
  font-size: 0.82rem;
  font-variation-settings:
    "opsz" 14,
    "WONK" 1;
  font-feature-settings: "tnum";
}

.select {
  font-family: var(--sans);
  font-size: 0.97rem;
  padding: 0.95rem 1.1rem;
  background: var(--paper);
  border: 1px solid var(--rule);
  border-radius: 4px;
  color: var(--ink);
  width: 100%;
  appearance: none;
  background-image: url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='14' height='14' viewBox='0 0 24 24' fill='none' stroke='%237a6346' stroke-width='2'><polyline points='6 9 12 15 18 9'/></svg>");
  background-repeat: no-repeat;
  background-position: right 1rem center;
  padding-right: 2.5rem;
  cursor: pointer;
}
.select:focus {
  outline: none;
  border-color: var(--accent);
}

.flash-action {
  gap: 0.9rem;
}
.flash-note {
  font-size: 0.82rem;
  color: var(--ink-3);
  font-style: italic;
  margin: 0;
  font-family: var(--serif);
  font-variation-settings:
    "opsz" 14,
    "SOFT" 50;
}

.flash-aside {
  background: var(--ink);
  color: var(--paper-deep);
  border-radius: 6px;
  padding: clamp(1.5rem, 3vw, 2.4rem);
  display: flex;
  flex-direction: column;
  gap: 1.4rem;
}
.flash-aside h4 {
  font-family: var(--serif);
  font-size: 1.4rem;
  font-weight: 380;
  margin: 0;
  color: var(--cream);
  font-variation-settings:
    "opsz" 30,
    "SOFT" 40;
}
.flash-steps {
  list-style: none;
  counter-reset: step;
  padding: 0;
  margin: 0;
  display: grid;
  gap: 0.85rem;
}
.flash-steps li {
  counter-increment: step;
  font-size: 0.92rem;
  line-height: 1.55;
  padding-left: 2rem;
  position: relative;
  color: var(--paper-deep);
}
.flash-steps li::before {
  content: counter(step, decimal-leading-zero);
  position: absolute;
  left: 0;
  top: 0;
  font-family: var(--serif);
  font-style: italic;
  color: var(--accent-soft);
  font-variation-settings:
    "opsz" 14,
    "WONK" 1;
  font-feature-settings: "tnum";
  font-size: 0.85rem;
}
.flash-steps li code {
  font-family: ui-monospace, "SF Mono", monospace;
  font-size: 0.85em;
  background: var(--espresso);
  padding: 0.05em 0.4em;
  border-radius: 3px;
  color: var(--accent-soft);
}
.flash-steps li em {
  font-family: var(--serif);
  font-style: italic;
  color: var(--cream);
  font-variation-settings:
    "opsz" 14,
    "SOFT" 50;
}
.flash-board-meta {
  margin-top: auto;
  padding-top: 1.4rem;
  border-top: 1px solid rgba(244, 234, 214, 0.2);
  display: grid;
  gap: 0.25rem;
}
.flash-board-name {
  font-family: var(--serif);
  font-size: 1.05rem;
  color: var(--cream);
  font-variation-settings:
    "opsz" 24,
    "SOFT" 30;
}
.flash-board-spec {
  font-size: 0.78rem;
  letter-spacing: 0.1em;
  text-transform: uppercase;
  color: var(--accent-soft);
}

/* esp-web-install-button styling override */
:deep(esp-web-install-button) {
  display: inline-block;
}

/* ───────────────────────────────  COMPANION APP  ─────────────────────────────── */
.section-app {
  padding-left: 0;
  padding-right: 0;
  max-width: none;
}
.app-canvas {
  background: linear-gradient(135deg, #1f140a 0%, #3d2817 55%, #5a3a20 100%);
  color: var(--cream);
  padding: clamp(3rem, 7vw, 6rem) clamp(1.5rem, 5vw, 4rem);
  position: relative;
  overflow: hidden;
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  gap: clamp(2rem, 5vw, 5rem);
  align-items: center;
  max-width: 1480px;
  margin: 0 auto;
  border-radius: 6px;
}
.app-canvas::after {
  content: "";
  position: absolute;
  inset: 0;
  background: radial-gradient(circle at 20% 110%, rgba(232, 200, 154, 0.14), transparent 55%);
  pointer-events: none;
}
@media (max-width: 880px) {
  .app-canvas {
    grid-template-columns: 1fr;
    padding: 2.4rem 1.5rem;
  }
}
.app-graphic {
  width: 100%;
  height: auto;
  border-radius: 4px;
  box-shadow: 0 30px 60px -20px rgba(0, 0, 0, 0.5);
  position: relative;
  z-index: 1;
}
.app-text {
  position: relative;
  z-index: 1;
}
.section-head-light {
  color: var(--accent-soft);
}
.section-head-light .section-kicker {
  color: var(--accent-soft);
}
.section-head-light::before {
  background: rgba(232, 200, 154, 0.4);
}
.section-title-light {
  color: var(--cream);
}
.section-title-light em.wonk {
  color: var(--accent-soft);
}
.section-lede-light {
  color: rgba(244, 234, 214, 0.78);
}

.app-stores {
  display: flex;
  flex-wrap: wrap;
  gap: 0.85rem;
  margin: 0 0 2rem;
}
.store {
  display: inline-flex;
  align-items: center;
  gap: 0.85rem;
  padding: 0.95rem 1.4rem;
  background: rgba(244, 234, 214, 0.06);
  border: 1px solid rgba(244, 234, 214, 0.2);
  border-radius: 4px;
  text-decoration: none;
  color: var(--cream);
  transition:
    background 0.25s,
    border-color 0.25s;
}
.store:hover {
  background: rgba(244, 234, 214, 0.12);
  border-color: var(--accent-soft);
}
.store > div {
  display: grid;
  gap: 0.1rem;
}
.store-os {
  font-family: var(--serif);
  font-size: 1.05rem;
  font-variation-settings:
    "opsz" 24,
    "SOFT" 30;
}
.store-track {
  font-size: 0.7rem;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: var(--accent-soft);
}
.app-features {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem 0.9rem;
}
.app-feature {
  display: inline-flex;
  align-items: center;
  gap: 0.4rem;
  font-size: 0.82rem;
  color: rgba(244, 234, 214, 0.7);
}

/* ───────────────────────────────  ECOSYSTEM  ─────────────────────────────── */
.eco-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
  gap: 1.25rem;
}
.eco {
  text-decoration: none;
  color: inherit;
  background: var(--paper-soft);
  border: 1px solid var(--rule);
  border-radius: 6px;
  padding: 1.6rem;
  display: flex;
  flex-direction: column;
  gap: 0.65rem;
  transition:
    border-color 0.25s,
    transform 0.25s,
    box-shadow 0.25s;
  position: relative;
}
.eco:hover {
  border-color: var(--accent);
  transform: translateY(-3px);
  box-shadow: 0 18px 40px -22px rgba(43, 29, 15, 0.24);
}
.eco-tag {
  align-self: start;
  font-size: 0.68rem;
  letter-spacing: 0.18em;
  text-transform: uppercase;
  color: var(--accent-deep);
  background: var(--paper-deep);
  padding: 0.3rem 0.65rem;
  border-radius: 999px;
}
.eco-title {
  font-family: var(--serif);
  font-size: 1.6rem;
  font-weight: 380;
  letter-spacing: -0.015em;
  margin: 0.2rem 0 0;
  color: var(--ink);
  font-variation-settings:
    "opsz" 36,
    "SOFT" 30;
}
.eco-blurb {
  font-size: 0.93rem;
  line-height: 1.55;
  color: var(--ink-2);
  margin: 0;
  flex: 1;
}
.eco-arrow {
  font-family: var(--serif);
  font-style: italic;
  color: var(--accent-deep);
  font-size: 0.92rem;
  margin-top: 0.5rem;
  font-variation-settings:
    "opsz" 14,
    "WONK" 1;
}

/* ───────────────────────────────  FOOTER  ─────────────────────────────── */
.footer {
  position: relative;
  z-index: 1;
  background: var(--espresso);
  color: var(--paper-deep);
  padding: clamp(3rem, 5vw, 4.5rem) clamp(1.25rem, 5vw, 4rem) 2rem;
  margin-top: clamp(3rem, 6vw, 6rem);
}
.footer-grid {
  max-width: 1400px;
  margin: 0 auto;
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 2rem 4rem;
  align-items: start;
}
@media (max-width: 720px) {
  .footer-grid {
    grid-template-columns: 1fr;
  }
}
.footer-brand {
  display: inline-flex;
  align-items: center;
  gap: 1rem;
}
.footer-mark {
  width: 44px;
  height: 44px;
  border-radius: 8px;
}
.footer-wordmark {
  font-family: var(--serif);
  font-size: 1.4rem;
  font-weight: 500;
  color: var(--cream);
  font-variation-settings:
    "opsz" 24,
    "SOFT" 30;
}
.footer-tag {
  font-style: italic;
  font-family: var(--serif);
  color: var(--accent-soft);
  font-size: 0.92rem;
  font-variation-settings:
    "opsz" 14,
    "SOFT" 50;
}
.footer-links {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem 1.6rem;
  align-self: center;
}
.footer-links a {
  color: var(--paper-deep);
  text-decoration: none;
  font-size: 0.88rem;
  letter-spacing: 0.04em;
  transition: color 0.25s;
}
.footer-links a:hover {
  color: var(--accent-soft);
}
.footer-rule {
  height: 1px;
  background: rgba(244, 234, 214, 0.12);
  margin: 2rem auto;
  max-width: 1400px;
}
.footer-bottom {
  max-width: 1400px;
  margin: 0 auto;
  display: flex;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 0.5rem;
  font-size: 0.78rem;
  color: rgba(244, 234, 214, 0.55);
  letter-spacing: 0.02em;
}
</style>

<style>
/* Global resets so embedded Vuetify/web components inherit the warm palette */
html,
body {
  background: #f5efe2;
}
body {
  margin: 0;
}
.v-application {
  background: transparent !important;
}

esp-web-install-button button[slot="activate"] {
  font: inherit;
}
</style>
