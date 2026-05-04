import "vuetify/styles";
import { createVuetify } from "vuetify";
import { aliases as builtinAliases, mdi } from "vuetify/iconsets/mdi-svg";
import { aliases as appAliases } from "./icons";

// Use the SVG iconset (mdi-svg) so we can ship the icons offline. Templates
// reference icons via "$mdi-foo" aliases — the @mdi/js paths are bundled at
// build time, only the icons we actually use end up in the JS output. No
// font CSS / no woff2 / works in AP mode without internet.
export default createVuetify({
  icons: {
    defaultSet: "mdi",
    aliases: { ...builtinAliases, ...appAliases },
    sets: { mdi },
  },
  theme: {
    defaultTheme: "light",
    themes: {
      light: {
        colors: {
          primary: "#ce9160",
          secondary: "#424242",
          accent: "#82B1FF",
          error: "#982f2f",
          info: "#2f6398",
          success: "#2f9852",
          warning: "#987e2f",
        },
      },
    },
  },
  defaults: {
    VBtn: {
      variant: "flat",
    },
    VCard: {
      elevation: 2,
    },
  },
});
