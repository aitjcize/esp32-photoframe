import { createApp } from "vue";
import { createPinia } from "pinia";
import vuetify from "./plugins/vuetify";
import router from "./router";
import App from "./App.vue";
// The icons and the font ship with the firmware (a frame has no internet access in its own hotspot, on a
// LAN without outbound access, ...): the page used to load them from cdn.jsdelivr.net and fonts.googleapis.com.
import "@mdi/font/css/materialdesignicons.css";
import "@fontsource/roboto/latin-300.css";
import "@fontsource/roboto/latin-400.css";
import "@fontsource/roboto/latin-500.css";
import "@fontsource/roboto/latin-700.css";

const app = createApp(App);
const pinia = createPinia();

app.use(pinia);
app.use(router);
app.use(vuetify);
app.mount("#app");
