import { createApp } from "vue";
import { createPinia } from "pinia";
import vuetify from "./plugins/vuetify";
import LandingPage from "./views/LandingPage.vue";

const app = createApp(LandingPage);

app.use(createPinia());
app.use(vuetify);

app.mount("#app");
