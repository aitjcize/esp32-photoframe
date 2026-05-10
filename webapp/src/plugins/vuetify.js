import "vuetify/styles";
import { createVuetify } from "vuetify";

export default createVuetify({
  theme: {
    defaultTheme: "light",
    themes: {
      light: {
        colors: {
          background: "#f5efe2",
          surface: "#faf6ec",
          primary: "#c98a3b",
          secondary: "#2b1d0f",
          accent: "#a06a26",
          error: "#8b3a2b",
          info: "#3a5a7a",
          success: "#5a7a3a",
          warning: "#a8742b",
        },
      },
    },
  },
  defaults: {
    VBtn: {
      variant: "flat",
    },
    VCard: {
      elevation: 0,
    },
  },
});
