#include "cert_pin.h"

#include <string.h>

#include "config_manager.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "cert_pin";

esp_err_t cert_pin_fetch_and_store(const char *url, char *err_out, size_t err_out_len)
{
    if (err_out && err_out_len > 0)
        err_out[0] = '\0';

    if (!url || url[0] == '\0') {
        if (err_out)
            strncpy(err_out, "URL is empty", err_out_len - 1);
        return ESP_FAIL;
    }

    // Parse host + port
    char host[256] = {0};
    int port = 443;
    const char *host_start = strstr(url, "://");
    if (!host_start) {
        if (err_out)
            strncpy(err_out, "Invalid URL format", err_out_len - 1);
        return ESP_FAIL;
    }
    host_start += 3;
    const char *host_end = strchr(host_start, '/');
    if (!host_end)
        host_end = host_start + strlen(host_start);
    const char *port_sep = strchr(host_start, ':');
    if (port_sep && port_sep < host_end) {
        size_t host_len = port_sep - host_start;
        if (host_len >= sizeof(host))
            host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
        port = atoi(port_sep + 1);
    } else {
        size_t host_len = host_end - host_start;
        if (host_len >= sizeof(host))
            host_len = sizeof(host) - 1;
        strncpy(host, host_start, host_len);
    }

    ESP_LOGI(TAG, "Fetching TLS certificate from %s:%d", host, port);

    esp_tls_cfg_t tls_cfg = {0};
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        if (err_out)
            strncpy(err_out, "TLS init failed", err_out_len - 1);
        return ESP_FAIL;
    }

    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &tls_cfg, tls);
    if (ret != 1) {
        int esp_tls_err = 0;
        int mbedtls_err = 0;
        esp_tls_error_handle_t error_handle = NULL;
        esp_tls_get_error_handle(tls, &error_handle);
        if (error_handle) {
            esp_tls_get_and_clear_last_error(error_handle, &esp_tls_err, &mbedtls_err);
        }
        ESP_LOGE(TAG, "TLS connection failed to %s:%d (esp_tls=0x%x, mbedtls=-0x%04x)", host, port,
                 esp_tls_err, -mbedtls_err);
        esp_tls_conn_destroy(tls);
        if (err_out) {
            snprintf(err_out, err_out_len,
                     "TLS handshake failed with %s:%d (mbedtls error -0x%04x)", host, port,
                     -mbedtls_err);
        }
        return ESP_FAIL;
    }

    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *) esp_tls_get_ssl_context(tls);
    const mbedtls_x509_crt *peer_cert = mbedtls_ssl_get_peer_cert(ssl);
    if (!peer_cert) {
        esp_tls_conn_destroy(tls);
        if (err_out)
            strncpy(err_out, "No certificate received from server", err_out_len - 1);
        return ESP_FAIL;
    }

    // Pin the issuer (next cert in the chain), not the leaf, so standard mbedtls
    // chain verification succeeds during subsequent HTTPS fetches: leaf is
    // signed by issuer, and the issuer matches our trust anchor.
    const mbedtls_x509_crt *issuer = peer_cert->next;
    if (!issuer) {
        esp_tls_conn_destroy(tls);
        if (err_out)
            strncpy(err_out,
                    "Server sent only a leaf certificate; pinning requires an intermediate",
                    err_out_len - 1);
        return ESP_FAIL;
    }

    size_t cert_der_len = issuer->raw.len;
    unsigned char *cert_der = malloc(cert_der_len);
    if (!cert_der) {
        esp_tls_conn_destroy(tls);
        if (err_out)
            strncpy(err_out, "Out of memory", err_out_len - 1);
        return ESP_FAIL;
    }
    memcpy(cert_der, issuer->raw.p, cert_der_len);

    esp_tls_conn_destroy(tls);

    config_manager_set_ca_cert_der(cert_der, cert_der_len);
    config_manager_touch_config();

    free(cert_der);

    ESP_LOGI(TAG, "Pinned issuer cert for %s:%d (%zu bytes)", host, port, cert_der_len);
    return ESP_OK;
}

void cert_pin_clear(void)
{
    config_manager_set_ca_cert_der(NULL, 0);
    config_manager_touch_config();
}
