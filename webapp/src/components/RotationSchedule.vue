<script setup>
import { ref, computed, watch } from "vue";
import {
  newCard,
  cardsFromCron,
  cardFromCron,
  compileCards,
  compileCard,
  describeCard,
  nextRuns,
  isValidCron,
  DAY_LABELS,
} from "../utils/cron";

const props = defineProps({
  modelValue: { type: Array, default: () => [] },
  disabled: { type: Boolean, default: false },
});
const emit = defineEmits(["update:modelValue"]);

const cards = ref(cardsFromCron(props.modelValue));

// Emit compiled cron whenever the cards change; re-derive cards when the bound
// value changes externally (e.g. after loading config), guarding against loops.
// The syncing flag swallows the watcher firing caused by that re-derivation, so
// loading a non-canonical stored rule doesn't emit a normalized copy and flag
// the settings as modified with zero user edits.
let syncing = false;
watch(
  cards,
  () => {
    if (syncing) {
      syncing = false;
      return;
    }
    emit("update:modelValue", compileCards(cards.value));
  },
  { deep: true }
);
watch(
  () => props.modelValue,
  (val) => {
    if (JSON.stringify(compileCards(cards.value)) !== JSON.stringify(val || [])) {
      syncing = true;
      cards.value = cardsFromCron(val || []);
    }
  }
);

const totalRules = computed(() => compileCards(cards.value).length);
const overBudget = computed(() => totalRules.value > 7);

const upcoming = computed(() =>
  nextRuns(compileCards(cards.value), new Date(), 4).map((d) =>
    d.toLocaleString([], {
      weekday: "short",
      hour: "2-digit",
      minute: "2-digit",
    })
  )
);

// Day chips shown Monday-first; values are cron dow (0 = Sunday).
const dayChips = [1, 2, 3, 4, 5, 6, 0].map((v) => ({ value: v, label: DAY_LABELS[v] }));

const daysOptions = [
  { value: "everyday", title: "Every day" },
  { value: "weekdays", title: "Weekdays" },
  { value: "weekends", title: "Weekends" },
  { value: "custom", title: "Custom" },
];

const hourItems = Array.from({ length: 24 }, (_, h) => ({
  value: h,
  title: `${String(h).padStart(2, "0")}:00`,
}));

// The device can't express an hour window that wraps past midnight, so "To"
// never offers hours before "From" (use two schedules for overnight windows).
function toHourItems(card) {
  return hourItems.filter((h) => h.value >= card.fromHour);
}
function setFromHour(card, v) {
  card.fromHour = v;
  if (card.toHour < v) card.toHour = v;
}

function everyItems(unit, current) {
  const base = unit === "hours" ? [1, 2, 3, 4, 6, 8, 12] : [5, 10, 15, 20, 30];
  // A hand-entered rule may use a step that isn't a preset; keep it selectable.
  const vals = base.includes(current) ? base : [...base, current].sort((a, b) => a - b);
  return vals.map((v) => ({ value: v, title: `${v}` }));
}

function daysModel(card) {
  return Array.isArray(card.days) ? "custom" : card.days;
}
function setDaysMode(card, mode) {
  card.days = mode === "custom" ? (Array.isArray(card.days) ? card.days : [1, 2, 3, 4, 5]) : mode;
}
function toggleDay(card, value) {
  if (!Array.isArray(card.days)) card.days = [];
  const i = card.days.indexOf(value);
  if (i >= 0) {
    // Keep at least one day selected — an empty selection would compile to
    // "every day", the opposite of what deselecting everything suggests.
    if (card.days.length > 1) card.days.splice(i, 1);
  } else {
    card.days.push(value);
  }
}

function addTime(card) {
  card.times = [...card.times, "12:00"].sort();
}
function removeTime(card, idx) {
  card.times.splice(idx, 1);
  if (card.times.length === 0) card.times.push("12:00");
}

function addCard() {
  cards.value.push(newCard());
}
function removeCard(idx) {
  cards.value.splice(idx, 1);
  if (cards.value.length === 0) cards.value.push(newCard());
}

// Advanced raw editing
function toggleAdvanced(card) {
  if (card.raw === null) {
    const rules = compileCard(card);
    card.raw = rules[0] || "0 */12 *";
    // A times card with several distinct minutes compiles to several rules;
    // split the extras into their own advanced cards so none are lost.
    if (rules.length > 1) {
      const idx = cards.value.indexOf(card);
      const extras = rules.slice(1).map((r) => {
        const c = newCard();
        c.raw = r;
        return c;
      });
      cards.value.splice(idx + 1, 0, ...extras);
    }
  } else {
    // Re-derive the builder state from the edited expression when possible;
    // unmappable or invalid expressions stay in advanced mode.
    Object.assign(card, cardFromCron(card.raw));
  }
}
function rawValid(card) {
  return card.raw === null || isValidCron(card.raw);
}
</script>

<template>
  <div>
    <div v-for="(card, idx) in cards" :key="idx" class="mb-4">
      <v-card variant="tonal" :disabled="disabled">
        <v-card-text>
          <div class="d-flex align-center mb-2">
            <span class="text-subtitle-2">Schedule {{ idx + 1 }}</span>
            <v-spacer />
            <v-btn
              v-if="cards.length > 1"
              icon="mdi-delete"
              variant="text"
              size="small"
              @click="removeCard(idx)"
            />
          </div>

          <!-- Advanced raw mode -->
          <template v-if="card.raw !== null">
            <v-text-field
              v-model="card.raw"
              label="Cron expression"
              variant="outlined"
              density="compact"
              hint="minute hour day-of-week"
              persistent-hint
              :error="!rawValid(card)"
              :error-messages="rawValid(card) ? [] : ['Invalid cron expression']"
            />
          </template>

          <!-- Friendly builder -->
          <template v-else>
            <!-- Days -->
            <v-btn-toggle
              :model-value="daysModel(card)"
              color="primary"
              density="compact"
              variant="outlined"
              divided
              class="mb-3 me-4 flex-wrap"
              @update:model-value="(v) => v && setDaysMode(card, v)"
            >
              <v-btn v-for="o in daysOptions" :key="o.value" :value="o.value" size="small">
                {{ o.title }}
              </v-btn>
            </v-btn-toggle>

            <div v-if="Array.isArray(card.days)" class="mb-3">
              <v-chip
                v-for="d in dayChips"
                :key="d.value"
                :color="card.days.includes(d.value) ? 'primary' : undefined"
                :variant="card.days.includes(d.value) ? 'flat' : 'outlined'"
                size="small"
                class="mr-1 mb-1"
                @click="toggleDay(card, d.value)"
              >
                {{ d.label }}
              </v-chip>
            </div>

            <!-- Mode -->
            <v-btn-toggle
              v-model="card.mode"
              color="primary"
              density="compact"
              variant="outlined"
              divided
              mandatory
              class="mb-3"
            >
              <v-btn value="interval" size="small">Throughout the day</v-btn>
              <v-btn value="times" size="small">At specific times</v-btn>
            </v-btn-toggle>

            <!-- Interval mode -->
            <v-row v-if="card.mode === 'interval'" dense align="center">
              <v-col cols="6" sm="2">
                <v-select
                  v-model="card.every"
                  :items="everyItems(card.unit, card.every)"
                  label="Every"
                  variant="outlined"
                  density="compact"
                  hide-details
                />
              </v-col>
              <v-col cols="6" sm="3">
                <v-select
                  v-model="card.unit"
                  :items="[
                    { value: 'minutes', title: 'minutes' },
                    { value: 'hours', title: 'hours' },
                  ]"
                  label="Unit"
                  variant="outlined"
                  density="compact"
                  hide-details
                />
              </v-col>
              <v-col cols="6" sm="3">
                <v-select
                  :model-value="card.fromHour"
                  :items="hourItems"
                  label="From"
                  variant="outlined"
                  density="compact"
                  hide-details
                  @update:model-value="(v) => setFromHour(card, v)"
                />
              </v-col>
              <v-col cols="6" sm="3">
                <v-select
                  v-model="card.toHour"
                  :items="toHourItems(card)"
                  label="To"
                  variant="outlined"
                  density="compact"
                  hide-details
                />
              </v-col>
            </v-row>

            <!-- Specific times -->
            <div v-else>
              <div class="d-flex flex-wrap align-center ga-2">
                <v-text-field
                  v-for="(t, ti) in card.times"
                  :key="ti"
                  v-model="card.times[ti]"
                  type="time"
                  variant="outlined"
                  density="compact"
                  hide-details
                  style="max-width: 130px"
                  append-inner-icon="mdi-close"
                  @click:append-inner="removeTime(card, ti)"
                />
                <v-btn variant="text" size="small" prepend-icon="mdi-plus" @click="addTime(card)">
                  Add time
                </v-btn>
              </div>
            </div>
          </template>

          <div class="text-caption text-medium-emphasis mt-3">{{ describeCard(card) }}</div>

          <v-btn variant="text" size="x-small" class="mt-1 px-0" @click="toggleAdvanced(card)">
            {{ card.raw !== null ? "Use builder" : "Advanced (cron)" }}
          </v-btn>
        </v-card-text>
      </v-card>
    </div>

    <v-btn
      variant="outlined"
      size="small"
      prepend-icon="mdi-plus"
      :disabled="disabled || totalRules >= 7"
      @click="addCard"
    >
      Add another schedule
    </v-btn>

    <v-alert v-if="overBudget" type="warning" variant="tonal" density="compact" class="mt-3">
      This uses {{ totalRules }} rules, but the device supports at most 7. Remove a schedule or
      simplify the specific times.
    </v-alert>

    <div class="mt-4">
      <div class="text-caption text-medium-emphasis mb-1">Upcoming rotations</div>
      <template v-if="upcoming.length">
        <v-chip v-for="(r, i) in upcoming" :key="i" size="small" class="mr-1 mb-1" variant="tonal">
          {{ r }}
        </v-chip>
        <div class="text-caption text-disabled mt-1">
          Times shown in this browser's timezone; the device follows its own timezone setting.
        </div>
      </template>
      <div v-else class="text-caption text-disabled">No upcoming rotations for this schedule.</div>
    </div>
  </div>
</template>
