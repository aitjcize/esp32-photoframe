<script setup>
import { ref } from "vue";
import { useAppStore } from "../stores";

const appStore = useAppStore();

const newAlbumDialog = ref(false);
const newAlbumName = ref("");
const deleteAlbumDialog = ref(false);
const albumToDelete = ref(null);
const displayLoading = ref(false);
const displayDialog = ref(false);
const imageToDisplay = ref(null);
const deleteImageDialog = ref(false);
const imageToDelete = ref(null);

async function createAlbum() {
  if (newAlbumName.value.trim()) {
    await appStore.createAlbum(newAlbumName.value);
    newAlbumName.value = "";
    newAlbumDialog.value = false;
  }
}

function confirmDeleteAlbum(album) {
  albumToDelete.value = album;
  deleteAlbumDialog.value = true;
}

async function deleteAlbum() {
  if (albumToDelete.value) {
    await appStore.deleteAlbum(albumToDelete.value.name);
    albumToDelete.value = null;
    deleteAlbumDialog.value = false;
  }
}

function confirmDisplayImage(image) {
  imageToDisplay.value = image;
  displayDialog.value = true;
}

async function displayImage() {
  if (imageToDisplay.value) {
    displayDialog.value = false;
    displayLoading.value = true;
    await appStore.displayImage(imageToDisplay.value.album, imageToDisplay.value.filename);
    displayLoading.value = false;
    imageToDisplay.value = null;
  }
}

function confirmDeleteImage(image) {
  imageToDelete.value = image;
  deleteImageDialog.value = true;
}

async function deleteImage() {
  if (imageToDelete.value) {
    await appStore.deleteImage(imageToDelete.value.album, imageToDelete.value.filename);
    imageToDelete.value = null;
    deleteImageDialog.value = false;
  }
}

function getThumbnailUrl(image) {
  return `/api/image?filepath=${encodeURIComponent(image.album + "/" + image.thumbnail)}`;
}
</script>

<template>
  <v-card>
    <v-card-title class="d-flex align-center">
      <v-icon icon="$mdi-image-multiple" class="mr-2" />
      Albums & Gallery
    </v-card-title>

    <v-card-text>
      <!-- Album List -->
      <div class="d-flex align-center mb-2">
        <span class="text-body-2 text-grey">
          ✓ = Enabled for auto-rotation • Click album name to view images
        </span>
        <v-spacer />
        <v-btn color="primary" size="small" @click="newAlbumDialog = true">
          <v-icon icon="$mdi-plus" start />
          New Album
        </v-btn>
      </div>

      <div class="d-flex align-center flex-wrap gap-2 mb-4">
        <div
          v-for="album in appStore.sortedAlbums"
          :key="album.name"
          class="album-chip d-flex align-center"
          :class="{ 'album-chip--selected': appStore.selectedAlbum === album.name }"
        >
          <v-checkbox-btn
            :model-value="album.enabled"
            color="primary"
            hide-details
            density="compact"
            @click.stop
            @update:model-value="appStore.toggleAlbumEnabled(album.name, $event)"
          />
          <span class="album-name" @click="appStore.selectAlbum(album.name)">
            {{ album.name }}
          </span>
          <v-btn
            v-if="album.name !== 'Default'"
            icon
            size="x-small"
            variant="text"
            @click.stop="confirmDeleteAlbum(album)"
          >
            <v-icon icon="$mdi-close" size="small" />
          </v-btn>
        </div>
      </div>

      <v-divider class="my-4" />

      <!-- Image Gallery -->
      <div class="d-flex align-center mb-4">
        <h3 class="text-h6">
          {{ appStore.selectedAlbum }}
        </h3>
        <v-spacer />
        <v-progress-circular v-if="displayLoading" indeterminate size="24" class="mr-2" />
        <span v-if="displayLoading" class="text-body-2 text-grey"> Updating display... </span>
      </div>

      <div v-if="appStore.loading.images" class="d-flex justify-center align-center py-12">
        <v-progress-circular indeterminate color="primary" />
      </div>

      <template v-else>
        <v-row v-if="appStore.currentAlbumImages.length > 0">
          <v-col
            v-for="image in appStore.currentAlbumImages"
            :key="image.filename"
            cols="6"
            sm="4"
            md="3"
            lg="2"
          >
            <v-card variant="outlined" class="image-card">
              <v-img
                :src="getThumbnailUrl(image)"
                :alt="image.filename"
                aspect-ratio="1"
                contain
                class="cursor-pointer bg-grey-lighten-3"
                @click="confirmDisplayImage(image)"
              >
                <template #placeholder>
                  <div class="d-flex align-center justify-center fill-height">
                    <v-progress-circular indeterminate color="grey-lighten-4" />
                  </div>
                </template>
              </v-img>
              <div class="delete-hotspot">
                <v-btn
                  icon="$mdi-delete"
                  size="x-small"
                  color="error"
                  class="delete-overlay"
                  @click.stop="confirmDeleteImage(image)"
                />
              </div>
            </v-card>
          </v-col>
        </v-row>

        <v-alert v-else type="info" variant="tonal" class="mt-4">
          No images in this album. Upload images to get started.
        </v-alert>
      </template>
    </v-card-text>
  </v-card>

  <!-- New Album Dialog -->
  <v-dialog v-model="newAlbumDialog" max-width="400">
    <v-card>
      <v-card-title>Create New Album</v-card-title>
      <v-card-text>
        <v-text-field
          v-model="newAlbumName"
          label="Album Name"
          autofocus
          @keyup.enter="createAlbum"
        />
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="newAlbumDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="createAlbum"> Create </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Delete Album Dialog -->
  <v-dialog v-model="deleteAlbumDialog" max-width="400">
    <v-card>
      <v-card-title>Delete Album?</v-card-title>
      <v-card-text>
        Are you sure you want to delete "{{ albumToDelete?.name }}"? This will also delete all
        images in the album.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="deleteAlbumDialog = false"> Cancel </v-btn>
        <v-btn color="error" @click="deleteAlbum"> Delete </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Display Image Dialog -->
  <v-dialog v-model="displayDialog" max-width="400">
    <v-card>
      <v-card-title>
        <v-icon icon="$mdi-monitor" class="mr-2" />
        Display Image?
      </v-card-title>
      <v-card-text>
        <div class="mb-3">Show this image on the e-paper display?</div>
        <div class="d-flex justify-center">
          <img
            v-if="imageToDisplay"
            :src="getThumbnailUrl(imageToDisplay)"
            alt=""
            class="confirm-thumb"
          />
        </div>
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="displayDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="displayImage"> Display </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- Delete Image Dialog -->
  <v-dialog v-model="deleteImageDialog" max-width="400">
    <v-card>
      <v-card-title>
        <v-icon icon="$mdi-delete" color="error" class="mr-2" />
        Delete Image?
      </v-card-title>
      <v-card-text>
        <div class="mb-3">Are you sure you want to delete this image?</div>
        <div class="d-flex justify-center">
          <img
            v-if="imageToDelete"
            :src="getThumbnailUrl(imageToDelete)"
            alt=""
            class="confirm-thumb"
          />
        </div>
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="deleteImageDialog = false"> Cancel </v-btn>
        <v-btn color="error" @click="deleteImage"> Delete </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>

<style scoped>
.album-chip {
  border: 1px solid rgba(0, 0, 0, 0.12);
  border-radius: 20px;
  padding: 4px 4px 4px 4px;
  background: white;
  cursor: pointer;
  margin-right: 8px;
  margin-bottom: 8px;
}
.album-chip--selected {
  border-color: rgb(var(--v-theme-primary));
  background: rgb(var(--v-theme-primary) / 0.08);
}
.album-name {
  cursor: pointer;
  user-select: none;
  padding: 0 4px;
}
.image-card {
  transition:
    transform 0.2s,
    box-shadow 0.2s;
}
.image-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}
.cursor-pointer {
  cursor: pointer;
}
.delete-hotspot {
  position: absolute;
  top: 0;
  right: 0;
  width: 48px;
  height: 48px;
  z-index: 1;
}
.delete-overlay {
  position: absolute;
  top: 4px;
  right: 4px;
  opacity: 0;
  transition: opacity 0.2s;
}
.delete-hotspot:hover .delete-overlay {
  opacity: 1;
}
.confirm-thumb {
  max-width: 100%;
  max-height: 60vh;
  display: block;
}
</style>
