import "vuetify/styles";
import { createVuetify } from "vuetify";

export default createVuetify({
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
