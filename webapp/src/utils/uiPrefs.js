import { ref, watch } from "vue";

// Per-browser UI preferences, persisted to localStorage so they survive reloads.

const WIDE_EDIT_KEY = "photoframe.wideEdit";

function loadBool(key) {
  try {
    return localStorage.getItem(key) === "true";
  } catch {
    return false;
  }
}

// Wide side-by-side editing layout: when on, the upload/preview card breaks out
// to ~80vw and the image-processing controls sit next to the preview.
export const wideEdit = ref(loadBool(WIDE_EDIT_KEY));

watch(wideEdit, (value) => {
  try {
    localStorage.setItem(WIDE_EDIT_KEY, value ? "true" : "false");
  } catch {
    // localStorage can be unavailable (private mode / disabled); ignore.
  }
});
