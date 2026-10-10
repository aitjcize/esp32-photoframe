import { defineConfig } from "vite";
import vue from "@vitejs/plugin-vue";
import vuetify from "vite-plugin-vuetify";
import { resolve } from "path";
import { gzipSync } from "node:zlib";
import { readdirSync, readFileSync, writeFileSync } from "node:fs";

// The firmware embeds only the .gz files (main/CMakeLists.txt EMBED_FILES)
// and serves the stored bytes verbatim with Content-Encoding: gzip
// (http_server.c, wifi_provisioning.c) -- raw, the assets would waste
// ~800 KB of the 3.5 MB app partition. The originals stay in the output so
// `npm run preview` still serves the app.
function gzipOutput(outDir) {
  return {
    name: "gzip-output",
    apply: "build",
    closeBundle() {
      for (const dir of [outDir, resolve(outDir, "assets")]) {
        for (const name of readdirSync(dir)) {
          if (!/\.(html|js|css|svg)$/.test(name)) continue;
          const path = resolve(dir, name);
          writeFileSync(`${path}.gz`, gzipSync(readFileSync(path), { level: 9 }));
        }
      }
    },
  };
}

// The icon font and Roboto are bundled (src/main.js). Their stylesheets list every font format there ever
// was; only woff2 is kept (every browser the Web UI needs reads it), and the font files are inlined into
// the stylesheet (build.assetsInlineLimit below), so the firmware serves no extra file for them.
function woff2Only() {
  const FONT_CSS = /[\/](@mdi[\/]font[\/]css[\/]materialdesignicons|@fontsource[\/]roboto[\/]latin-\d+)\.css$/;
  return {
    name: "woff2-only",
    enforce: "pre",
    transform(code, id) {
      if (!FONT_CSS.test(id.split("?")[0])) return null;
      return code.replace(/@font-face\s*\{[^}]*\}/g, (block) => {
        const woff2 = block.match(
          /url\(\s*["']?([^"')]+\.woff2[^"')]*)["']?\s*\)\s*format\(\s*["']woff2["']\s*\)/
        );
        if (!woff2) return block;
        return block
          .replace(/\s*src:[^;]*;/g, "")
          .replace(/\}\s*$/, `  src: url("${woff2[1]}") format("woff2");
}`);
      });
    },
  };
}

export default defineConfig({
  plugins: [
    woff2Only(),
    vue(),
    vuetify({ autoImport: true }),
    gzipOutput(resolve(__dirname, "../main/webapp")),
  ],
  build: {
    outDir: resolve(__dirname, "../main/webapp"),
    emptyOutDir: true,
    // the bundled fonts go into the stylesheet (see woff2Only); every other asset keeps Vite's default
    assetsInlineLimit: (file) => (file.endsWith(".woff2") ? true : undefined),
    rollupOptions: {
      external: ["/measurement_sample.jpg"],
      output: {
        entryFileNames: "assets/[name].js",
        chunkFileNames: "assets/[name].js",
        assetFileNames: "assets/[name].[ext]",
      },
    },
  },
  server: {
    proxy: {
      "/api": {
        target: "http://192.168.0.140",
        changeOrigin: true,
      },
    },
    fs: {
      allow: [resolve(__dirname, "..")],
    },
  },
});
