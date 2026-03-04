#include "album_manager.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"


static const char *TAG = "album_manager";
static char enabled_albums_str[512] = "";

esp_err_t album_manager_init(void)
{
    if (!storage_has_persistent_storage()) {
        ESP_LOGI(TAG, "Storage not mounted - skipping album manager initialization");
        return ESP_OK;
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = sizeof(enabled_albums_str);
        if (nvs_get_str(nvs_handle, NVS_ENABLED_ALBUMS_KEY, enabled_albums_str, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded enabled albums from NVS: %s", enabled_albums_str);
        } else {
            ESP_LOGI(TAG, "No enabled albums in NVS, enabling default album");
            snprintf(enabled_albums_str, sizeof(enabled_albums_str), "%s", DEFAULT_ALBUM_NAME);
        }
        nvs_close(nvs_handle);
    }

    // Ensure image directory exists
    struct stat st;
    if (stat(IMAGE_DIRECTORY, &st) != 0) {
        ESP_LOGI(TAG, "Creating image directory: %s", IMAGE_DIRECTORY);
        mkdir(IMAGE_DIRECTORY, 0775);
    }

    esp_err_t ret = album_manager_ensure_default_album();
    if (ret == ESP_OK && strlen(enabled_albums_str) == 0) {
        album_manager_set_album_enabled(DEFAULT_ALBUM_NAME, true);
    }
    return ret;
}

esp_err_t album_manager_ensure_default_album(void)
{
    char default_album_path[256];
    snprintf(default_album_path, sizeof(default_album_path), "%s/%s", IMAGE_DIRECTORY,
             DEFAULT_ALBUM_NAME);

    struct stat st;
    if (stat(default_album_path, &st) != 0) {
        ESP_LOGI(TAG, "Creating default album: %s", default_album_path);
        if (mkdir(default_album_path, 0775) != 0) {
            ESP_LOGE(TAG, "Failed to create default album directory");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t album_manager_list_albums(char ***albums, int *count)
{
    if (!albums || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(IMAGE_DIRECTORY);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open image directory");
        return ESP_FAIL;
    }

    *count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            (*count)++;
        }
    }

    if (*count == 0) {
        closedir(dir);
        *albums = NULL;
        return ESP_OK;
    }

    *albums = malloc(*count * sizeof(char *));
    if (!*albums) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < *count) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            (*albums)[idx] = strdup(entry->d_name);
            if (!(*albums)[idx]) {
                album_manager_free_album_list(*albums, idx);
                closedir(dir);
                return ESP_ERR_NO_MEM;
            }
            idx++;
        }
    }

    closedir(dir);
    return ESP_OK;
}

void album_manager_free_album_list(char **albums, int count)
{
    if (!albums) {
        return;
    }

    for (int i = 0; i < count; i++) {
        if (albums[i]) {
            free(albums[i]);
        }
    }
    free(albums);
}

esp_err_t album_manager_create_album(const char *album_name)
{
    if (!album_name || strlen(album_name) == 0 || strchr(album_name, '/') != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char album_path[256];
    snprintf(album_path, sizeof(album_path), "%s/%s", IMAGE_DIRECTORY, album_name);

    struct stat st;
    if (stat(album_path, &st) == 0) {
        ESP_LOGW(TAG, "Album already exists: %s", album_name);
        return ESP_ERR_INVALID_STATE;
    }

    if (mkdir(album_path, 0775) != 0) {
        ESP_LOGE(TAG, "Failed to create album directory: %s", album_name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Created album: %s", album_name);
    return ESP_OK;
}

esp_err_t album_manager_delete_album(const char *album_name)
{
    if (!album_name || strlen(album_name) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(album_name, DEFAULT_ALBUM_NAME) == 0) {
        ESP_LOGE(TAG, "Cannot delete default album");
        return ESP_ERR_INVALID_ARG;
    }

    char album_path[256];
    snprintf(album_path, sizeof(album_path), "%s/%s", IMAGE_DIRECTORY, album_name);

    DIR *dir = opendir(album_path);
    if (!dir) {
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", album_path, entry->d_name);
        unlink(filepath);
    }
    closedir(dir);

    if (rmdir(album_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete album directory: %s", album_name);
        return ESP_FAIL;
    }

    album_manager_set_album_enabled(album_name, false);

    ESP_LOGI(TAG, "Deleted album: %s", album_name);
    return ESP_OK;
}

esp_err_t album_manager_set_album_enabled(const char *album_name, bool enabled)
{
    if (!album_name || strlen(album_name) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Only check existence when enabling (not when disabling during cleanup)
    if (enabled && !album_manager_album_exists(album_name)) {
        ESP_LOGE(TAG, "Album does not exist: %s", album_name);
        return ESP_ERR_NOT_FOUND;
    }

    char new_list[512] = "";
    bool found = false;
    char *token;
    char temp_str[512];
    strncpy(temp_str, enabled_albums_str, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0';

    token = strtok(temp_str, ",");
    while (token != NULL) {
        while (*token == ' ')
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ')
            end--;
        *(end + 1) = '\0';

        if (strcmp(token, album_name) == 0) {
            found = true;
            if (enabled) {
                if (strlen(new_list) > 0)
                    strcat(new_list, ",");
                strcat(new_list, album_name);
            }
        } else {
            if (strlen(new_list) > 0)
                strcat(new_list, ",");
            strcat(new_list, token);
        }
        token = strtok(NULL, ",");
    }

    if (!found && enabled) {
        if (strlen(new_list) > 0)
            strcat(new_list, ",");
        strcat(new_list, album_name);
    }

    strncpy(enabled_albums_str, new_list, sizeof(enabled_albums_str) - 1);
    enabled_albums_str[sizeof(enabled_albums_str) - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_ENABLED_ALBUMS_KEY, enabled_albums_str);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Set album %s to %s. Enabled albums: %s", album_name,
             enabled ? "enabled" : "disabled", enabled_albums_str);
    return ESP_OK;
}

bool album_manager_is_album_enabled(const char *album_name)
{
    if (!album_name || strlen(album_name) == 0) {
        return false;
    }

    char temp_str[512];
    strncpy(temp_str, enabled_albums_str, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0';

    char *token = strtok(temp_str, ",");
    while (token != NULL) {
        while (*token == ' ')
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ')
            end--;
        *(end + 1) = '\0';

        if (strcmp(token, album_name) == 0) {
            return true;
        }
        token = strtok(NULL, ",");
    }
    return false;
}

esp_err_t album_manager_get_enabled_albums(char ***albums, int *count)
{
    if (!albums || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    if (strlen(enabled_albums_str) == 0) {
        *albums = NULL;
        return ESP_OK;
    }

    char temp_str[512];
    strncpy(temp_str, enabled_albums_str, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0';

    char *token = strtok(temp_str, ",");
    while (token != NULL) {
        (*count)++;
        token = strtok(NULL, ",");
    }

    if (*count == 0) {
        *albums = NULL;
        return ESP_OK;
    }

    *albums = malloc(*count * sizeof(char *));
    if (!*albums) {
        return ESP_ERR_NO_MEM;
    }

    strncpy(temp_str, enabled_albums_str, sizeof(temp_str) - 1);
    temp_str[sizeof(temp_str) - 1] = '\0';

    int idx = 0;
    token = strtok(temp_str, ",");
    while (token != NULL && idx < *count) {
        while (*token == ' ')
            token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ')
            end--;
        *(end + 1) = '\0';

        (*albums)[idx] = strdup(token);
        if (!(*albums)[idx]) {
            album_manager_free_album_list(*albums, idx);
            return ESP_ERR_NO_MEM;
        }
        idx++;
        token = strtok(NULL, ",");
    }

    return ESP_OK;
}

esp_err_t album_manager_get_album_path(const char *album_name, char *path, size_t path_len)
{
    if (!album_name || !path || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path, path_len, "%s/%s", IMAGE_DIRECTORY, album_name);
    return ESP_OK;
}

bool album_manager_album_exists(const char *album_name)
{
    if (!album_name || strlen(album_name) == 0) {
        return false;
    }

    char album_path[256];
    snprintf(album_path, sizeof(album_path), "%s/%s", IMAGE_DIRECTORY, album_name);

    struct stat st;
    return (stat(album_path, &st) == 0 && S_ISDIR(st.st_mode));
}