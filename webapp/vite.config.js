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

export default defineConfig({
  plugins: [
    vue(),
    vuetify({ autoImport: true }),
    gzipOutput(resolve(__dirname, "../main/webapp")),
  ],
  build: {
    outDir: resolve(__dirname, "../main/webapp"),
    emptyOutDir: true,
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
