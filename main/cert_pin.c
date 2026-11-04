#include "cert_pin.h"

#include <stdlib.h>
#include <string.h>

#include "config_manager.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "cert_pin";

static void set_err(char *err_out, size_t err_out_len, const char *msg)
{
    if (err_out && err_out_len > 0) {
        strncpy(err_out, msg, err_out_len - 1);
        err_out[err_out_len - 1] = '\0';
    }
}

static bool parse_host_port(const char *url, char *host, size_t host_size, int *port)
{
    const char *host_start = strstr(url, "://");
    if (!host_start) {
        return false;
    }
    host_start += 3;
    const char *host_end = strchr(host_start, '/');
    if (!host_end) {
        host_end = host_start + strlen(host_start);
    }
    const char *port_sep = strchr(host_start, ':');
    size_t host_len;
    if (port_sep && port_sep < host_end) {
        host_len = port_sep - host_start;
        *port = atoi(port_sep + 1);
    } else {
        host_len = host_end - host_start;
        *port = 443;
    }
    if (host_len >= host_size) {
        host_len = host_size - 1;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    return host[0] != '\0';
}

// Opens a TLS connection with `cfg`; on failure fills `err_out` with the
// mbedtls error and returns NULL.
static esp_tls_t *tls_connect(const char *host, int port, const esp_tls_cfg_t *cfg, char *err_out,
                              size_t err_out_len)
{
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        set_err(err_out, err_out_len, "TLS init failed");
        return NULL;
    }
    if (esp_tls_conn_new_sync(host, strlen(host), port, cfg, tls) == 1) {
        return tls;
    }
    // esp-tls hands back the mbedtls code already negated (a positive
    // number) and the X.509 verification flags.
    int mbedtls_code = 0;
    int verify_flags = 0;
    esp_tls_error_handle_t error_handle = NULL;
    esp_tls_get_error_handle(tls, &error_handle);
    if (error_handle) {
        esp_tls_get_and_clear_last_error(error_handle, &mbedtls_code, &verify_flags);
    }
    ESP_LOGE(TAG, "TLS connection failed to %s:%d (mbedtls=-0x%04x, verify flags=0x%x)", host, port,
             mbedtls_code, verify_flags);
    esp_tls_conn_destroy(tls);
    if (err_out && err_out_len > 0) {
        snprintf(err_out, err_out_len, "TLS handshake failed with %s:%d (mbedtls error -0x%04x)",
                 host, port, mbedtls_code);
    }
    return NULL;
}

static bool is_self_signed(const mbedtls_x509_crt *crt)
{
    char issuer[256];
    char subject[256];
    if (mbedtls_x509_dn_gets(issuer, sizeof(issuer), &crt->issuer) < 0 ||
        mbedtls_x509_dn_gets(subject, sizeof(subject), &crt->subject) < 0) {
        return false;
    }
    return strcmp(issuer, subject) == 0;
}

static esp_err_t capture(const char *host, int port, char *err_out, size_t err_out_len)
{
    esp_tls_cfg_t tls_cfg = {0};
    esp_tls_t *tls = tls_connect(host, port, &tls_cfg, err_out, err_out_len);
    if (!tls) {
        return ESP_FAIL;
    }

    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *) esp_tls_get_ssl_context(tls);
    const mbedtls_x509_crt *peer_cert = mbedtls_ssl_get_peer_cert(ssl);
    if (!peer_cert) {
        esp_tls_conn_destroy(tls);
        set_err(err_out, err_out_len, "No certificate received from server");
        return ESP_FAIL;
    }

    // Pin the issuer (next cert in the chain), not the leaf, so standard mbedtls
    // chain verification succeeds during subsequent HTTPS fetches: leaf is
    // signed by issuer, and the issuer matches our trust anchor. A self-signed
    // certificate is its own issuer and mbedtls trusts a leaf that matches a
    // trust anchor byte for byte, so that one is pinned as it is.
    const mbedtls_x509_crt *anchor = peer_cert->next;
    if (!anchor) {
        if (!is_self_signed(peer_cert)) {
            esp_tls_conn_destroy(tls);
            set_err(err_out, err_out_len,
                    "Server sent only its certificate and no public CA vouches for it; "
                    "pinning needs the issuing CA in the chain");
            return ESP_FAIL;
        }
        anchor = peer_cert;
    }

    size_t cert_der_len = anchor->raw.len;
    unsigned char *cert_der = malloc(cert_der_len);
    if (!cert_der) {
        esp_tls_conn_destroy(tls);
        set_err(err_out, err_out_len, "Out of memory");
        return ESP_FAIL;
    }
    memcpy(cert_der, anchor->raw.p, cert_der_len);
    esp_tls_conn_destroy(tls);

    config_manager_set_ca_cert_der(cert_der, cert_der_len);
    config_manager_touch_config();
    free(cert_der);

    ESP_LOGI(TAG, "Pinned %s cert for %s:%d (%zu bytes)",
             anchor == peer_cert ? "self-signed" : "issuer", host, port, cert_der_len);
    return ESP_OK;
}

esp_err_t cert_pin_fetch_and_store(const char *url, char *err_out, size_t err_out_len)
{
    set_err(err_out, err_out_len, "");
    if (!url || url[0] == '\0') {
        set_err(err_out, err_out_len, "URL is empty");
        return ESP_FAIL;
    }
    char host[256];
    int port;
    if (!parse_host_port(url, host, sizeof(host), &port)) {
        set_err(err_out, err_out_len, "Invalid URL format");
        return ESP_FAIL;
    }

    // A publicly trusted certificate is verified by the bundle on every fetch
    // and survives its renewals; a pin would only break at the next one (#142).
    esp_tls_cfg_t bundle_cfg = {.crt_bundle_attach = esp_crt_bundle_attach};
    char probe_err[128];
    esp_tls_t *tls = tls_connect(host, port, &bundle_cfg, probe_err, sizeof(probe_err));
    if (tls) {
        esp_tls_conn_destroy(tls);
        if (config_manager_has_ca_cert()) {
            cert_pin_clear();
        }
        ESP_LOGI(TAG, "%s:%d is verified by the CA bundle; nothing to pin", host, port);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "%s:%d is not verified by the CA bundle (%s); pinning its certificate", host,
             port, probe_err);
    return capture(host, port, err_out, err_out_len);
}

void cert_pin_clear(void)
{
    config_manager_set_ca_cert_der(NULL, 0);
    config_manager_touch_config();
}
