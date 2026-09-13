import { beforeEach, describe, expect, it, vi } from "vitest";
import { createPinia, setActivePinia } from "pinia";
import { useSettingsStore } from "./settings.js";

const AMSTERDAM = "CET-1CEST,M3.5.0/2,M10.5.0/3";

function configFixture(overrides = {}) {
  return {
    auto_rotate: true,
    rotate_cron: ["0 */12 *"],
    display_rotation_deg: 180,
    image_url: "https://loremflickr.com/800/480",
    deep_sleep_enabled: true,
    chime_enabled: true,
    chime_preset: "mozart",
    chime_url: "",
    chime_source: "preset",
    chime_pull_mode: "with_rotate",
    chime_play_when: "after",
    chime_file: "",
    debug_log_enabled: false,
    ha_url: "",
    save_downloaded_images: true,
    display_orientation: "landscape",
    rotation_mode: "storage",
    sd_rotation_mode: "random",
    device_name: "PhotoFrame",
    ntp_server: "pool.ntp.org",
    ip_mode: "dhcp",
    static_ip: "",
    static_netmask: "255.255.255.0",
    static_gateway: "",
    dns_server: "",
    wifi_ssid: "home",
    access_token: "",
    http_header_key: "",
    http_header_value: "",
    openai_api_key: "",
    google_api_key: "",
    timezone: AMSTERDAM,
    ...overrides,
  };
}

function mockFetch(data) {
  return vi.fn().mockImplementation(async (_url, options = {}) => {
    return {
      ok: true,
      headers: { get: () => "application/json" },
      json: async () => (options.method === "PATCH" ? { status: "success" } : data),
    };
  });
}

describe("settings store timezone", () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
  });

  it("loads CET/CEST as the POSIX string, not offset 0", async () => {
    const data = configFixture();
    vi.stubGlobal("fetch", mockFetch(data));
    const store = useSettingsStore();
    await store.loadDeviceSettings();
    expect(store.deviceSettings.timezone).toBe(AMSTERDAM);
    expect(store.deviceSettings.timezoneOffset).toBeNull();
  });

  it("save without edits does not PATCH timezone and does not rewrite UTC0", async () => {
    const data = configFixture();
    const fetchMock = mockFetch(data);
    vi.stubGlobal("fetch", fetchMock);
    const store = useSettingsStore();
    await store.loadDeviceSettings();

    const result = await store.saveDeviceSettings();
    expect(result.success).toBe(true);
    const patchCalls = fetchMock.mock.calls.filter(([, opts]) => opts && opts.method === "PATCH");
    expect(patchCalls).toHaveLength(0);
    expect(result.message).toBe("No changes to save");
    expect(store.deviceSettings.timezone).toBe(AMSTERDAM);
  });

  it("save after an unrelated edit still sends the POSIX timezone if compared equal", async () => {
    const data = configFixture();
    const fetchMock = mockFetch(data);
    vi.stubGlobal("fetch", fetchMock);
    const store = useSettingsStore();
    await store.loadDeviceSettings();
    store.deviceSettings.deviceName = "Woonkamer";

    const result = await store.saveDeviceSettings();
    expect(result.success).toBe(true);
    const patchCalls = fetchMock.mock.calls.filter(([, opts]) => opts && opts.method === "PATCH");
    expect(patchCalls).toHaveLength(1);
    const body = JSON.parse(patchCalls[0][1].body);
    expect(body.timezone).toBeUndefined();
    expect(body.device_name).toBe("Woonkamer");
  });
});
