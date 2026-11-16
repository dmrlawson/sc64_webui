// Tiny single-request-at-a-time HTTP/1.1 server and the file manager API.
//
// Requests are served one after another from the main loop in plain blocking
// style; every wait pumps plat_poll() so USB and the network keep moving.
// Every response uses "Connection: close".

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

#include "http.h"
#include "net.h"
#include "platform.h"
#include "sc64.h"
#include "storage.h"

#define HEADER_MAX 2048
#define PATH_MAX_LEN 1024
// File transfer buffer: one SD read/write and one FatFs call per IO_SIZE bytes.
#if PICO_RP2040
#define IO_SIZE (32 * 1024)   // Pico W: 264 kB of RAM
#else
#define IO_SIZE (64 * 1024)   // Pico 2 W: 520 kB of RAM
#endif
#define NET_TIMEOUT_MS 15000
#define DRAIN_LIMIT (1024 * 1024)
#define MAX_DEPTH 24

extern const unsigned char index_html_gz[];
extern const unsigned int index_html_gz_len;

typedef struct {
    net_conn_t *conn;
    char method[8];
    char *path;
    char *query;
    uint64_t content_length;
    bool has_length;    // the request carried a Content-Length header
    uint8_t *body;      // body bytes that arrived together with the header
    size_t body_len;
    uint64_t body_read; // body bytes consumed so far
} request_t;

static char header_buf[HEADER_MAX + 1];
static uint8_t io_buf[IO_SIZE] __attribute__((aligned(4)));
static char out_buf[2048];
static size_t out_len;
static char path_a[PATH_MAX_LEN];
static char path_b[PATH_MAX_LEN + 8];

// ---- blocking socket helpers ----

static bool send_all(net_conn_t *conn, const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t last = plat_millis();
    while (len > 0) {
        int n = net_send(conn, p, len);
        if (n < 0) {
            return false;
        }
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            last = plat_millis();
            continue;
        }
        if ((plat_millis() - last) > NET_TIMEOUT_MS) {
            return false;
        }
        plat_poll();
    }
    return true;
}

// Receive at least one byte (up to max). 0 on timeout/close.
static size_t recv_some(net_conn_t *conn, uint8_t *buf, size_t max) {
    uint32_t start = plat_millis();
    for (;;) {
        int n = net_recv(conn, buf, max);
        if (n > 0) {
            return (size_t)n;
        }
        if (n < 0 || (plat_millis() - start) > NET_TIMEOUT_MS) {
            return 0;
        }
        plat_poll();
    }
}

// Read up to max body bytes; returns 0 when the body is exhausted or on error.
static size_t body_read(request_t *req, uint8_t *buf, size_t max) {
    uint64_t left = req->content_length - req->body_read;
    if (left == 0) {
        return 0;
    }
    if (max > left) {
        max = (size_t)left;
    }
    size_t n;
    if (req->body_len > 0) {
        n = req->body_len < max ? req->body_len : max;
        memcpy(buf, req->body, n);
        req->body += n;
        req->body_len -= n;
    } else {
        n = recv_some(req->conn, buf, max);
    }
    req->body_read += n;
    return n;
}

// Swallow a (small) unread request body so the client reads our response
// instead of seeing a connection reset.
static void body_drain(request_t *req) {
    if ((req->content_length - req->body_read) > DRAIN_LIMIT) {
        return;
    }
    while (body_read(req, io_buf, IO_SIZE) > 0) {
    }
}

// ---- buffered output ----

static bool out_ok;

static void out_begin(void) {
    out_len = 0;
    out_ok = true;
}

static void out_flush(request_t *req) {
    if (out_ok && out_len > 0) {
        out_ok = send_all(req->conn, out_buf, out_len);
    }
    out_len = 0;
}

static void out_write(request_t *req, const char *data, size_t len) {
    while (len > 0) {
        size_t n = sizeof(out_buf) - out_len;
        if (n > len) {
            n = len;
        }
        memcpy(out_buf + out_len, data, n);
        out_len += n;
        data += n;
        len -= n;
        if (out_len == sizeof(out_buf)) {
            out_flush(req);
        }
    }
}

static void out_printf(request_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out_printf(request_t *req, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(tmp) - 1) {
        n = (int)sizeof(tmp) - 1;
    }
    out_write(req, tmp, (size_t)n);
}

static void out_json_string(request_t *req, const char *s) {
    out_write(req, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            out_write(req, esc, 2);
        } else if (c < 0x20) {
            out_printf(req, "\\u%04x", c);
        } else {
            out_write(req, (const char *)&c, 1);
        }
    }
    out_write(req, "\"", 1);
}

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
    }
    return "Error";
}

static void out_head(request_t *req, int code, const char *content_type, const char *extra) {
    out_begin();
    out_printf(req,
               "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\nCache-Control: no-store\r\n%s\r\n",
               code, status_text(code), content_type, extra ? extra : "");
}

static void reply_error(request_t *req, int code, const char *state, const char *message) {
    body_drain(req);
    out_head(req, code, "application/json", NULL);
    out_printf(req, "{\"error\":");
    out_json_string(req, message);
    if (state) {
        out_printf(req, ",\"state\":\"%s\"", state);
    }
    out_printf(req, "}");
    out_flush(req);
}

static void reply_ok(request_t *req) {
    out_head(req, 200, "application/json", NULL);
    out_printf(req, "{\"ok\":true}");
    out_flush(req);
}

static void reply_fresult(request_t *req, FRESULT fr) {
    switch (fr) {
        case FR_OK: reply_ok(req); return;
        case FR_NO_FILE:
        case FR_NO_PATH: reply_error(req, 404, NULL, "File or folder not found."); return;
        case FR_EXIST: reply_error(req, 409, NULL, "Something with that name already exists."); return;
        case FR_DENIED: reply_error(req, 403, NULL, "Access denied (read-only, or the card is full)."); return;
        case FR_INVALID_NAME: reply_error(req, 400, NULL, "That name is not valid on the SD card."); return;
        case FR_DISK_ERR:
        case FR_NOT_READY: reply_error(req, 500, NULL, "SD card I/O error. Is the SummerCart64 still connected?"); return;
        case FR_NO_FILESYSTEM: reply_error(req, 500, NULL, "No filesystem on the SD card."); return;
        default: break;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "Filesystem error %d.", (int)fr);
    reply_error(req, 500, NULL, msg);
}

// ---- request parsing ----

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Fetch and percent-decode a query parameter. False if missing or too long.
static bool query_get(const request_t *req, const char *key, char *out, size_t cap) {
    size_t key_len = strlen(key);
    const char *p = req->query;
    while (p && *p) {
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > key_len && p[key_len] == '=' && strncmp(p, key, key_len) == 0) {
            const char *v = p + key_len + 1;
            const char *v_end = p + len;
            size_t o = 0;
            while (v < v_end) {
                char c = *v++;
                if (c == '%' && (v_end - v) >= 2 && hex_value(v[0]) >= 0 && hex_value(v[1]) >= 0) {
                    c = (char)((hex_value(v[0]) << 4) | hex_value(v[1]));
                    v += 2;
                }
                if (c == '\0' || (o + 1) >= cap) {
                    return false;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        p = end ? end + 1 : NULL;
    }
    return false;
}

// Fetch an absolute SD card path parameter ("/", "/dir/file"). Trailing
// slashes are trimmed so FatFs accepts it.
static bool query_path(request_t *req, const char *key, char *out, size_t cap) {
    if (!query_get(req, key, out, cap) || out[0] != '/') {
        reply_error(req, 400, NULL, "Missing or invalid path.");
        return false;
    }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    return true;
}

static bool require_storage(request_t *req) {
    storage_state_t state = storage_acquire();
    if (state == STORAGE_READY) {
        return true;
    }
    reply_error(req, 503, storage_state_name(state), storage_state_message(state));
    return false;
}

static const char *header_value(const char *headers, const char *name) {
    size_t name_len = strlen(name);
    for (const char *line = headers; line && *line; ) {
        const char *next = strstr(line, "\r\n");
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            return v;
        }
        line = next ? next + 2 : NULL;
    }
    return NULL;
}

// ---- handlers ----

static void handle_index(request_t *req) {
    char extra[96];
    snprintf(extra, sizeof(extra), "Content-Encoding: gzip\r\nContent-Length: %u\r\n", index_html_gz_len);
    out_head(req, 200, "text/html; charset=utf-8", extra);
    out_flush(req);
    if (out_ok) {
        send_all(req->conn, index_html_gz, index_html_gz_len);
    }
}

static void handle_status(request_t *req) {
    storage_state_t state = storage_mounted() ? STORAGE_READY : storage_probe();
    out_head(req, 200, "application/json", NULL);
    out_printf(req, "{\"state\":\"%s\",\"message\":", storage_state_name(state));
    out_json_string(req, storage_state_message(state));
    out_printf(req, ",\"version\":\"%s\"", WEBUI_VERSION);
    if (state != STORAGE_NO_USB && state != STORAGE_NO_LINK) {
        sc64_time_t t;
        out_printf(req, ",\"firmware\":\"%s\"", sc64_version());
        if (sc64_time_get(&t)) {
            out_printf(req, ",\"rtc\":\"%04u-%02u-%02u %02u:%02u:%02u\"", t.year, t.month, t.day, t.hour,
                       t.minute, t.second);
        }
    }
    out_printf(req, "}");
    out_flush(req);
}

static void handle_log(request_t *req) {
    out_head(req, 200, "text/plain; charset=utf-8", NULL);
    size_t n = log_snapshot((char *)io_buf, IO_SIZE);
    out_write(req, (const char *)io_buf, n);
    out_flush(req);
}

static void handle_df(request_t *req) {
    if (!require_storage(req)) {
        return;
    }
    FATFS *fs;
    DWORD free_clusters;
    FRESULT fr = f_getfree("", &free_clusters, &fs);
    if (fr != FR_OK) {
        reply_fresult(req, fr);
        return;
    }
    uint64_t cluster_bytes = (uint64_t)fs->csize * SC64_SECTOR_SIZE;
    out_head(req, 200, "application/json", NULL);
    out_printf(req, "{\"total\":%llu,\"free\":%llu}",
               (unsigned long long)((uint64_t)(fs->n_fatent - 2) * cluster_bytes),
               (unsigned long long)((uint64_t)free_clusters * cluster_bytes));
    out_flush(req);
}

static void handle_list(request_t *req) {
    if (!query_path(req, "path", path_a, sizeof(path_a)) || !require_storage(req)) {
        return;
    }
    DIR dir;
    static FILINFO info;
    FRESULT fr = f_opendir(&dir, path_a);
    if (fr != FR_OK) {
        reply_fresult(req, fr);
        return;
    }
    out_head(req, 200, "application/json", NULL);
    out_printf(req, "{\"entries\":[");
    bool first = true;
    bool complete = false;
    while (out_ok) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK) {
            break;
        }
        if (info.fname[0] == '\0') {
            complete = true;
            break;
        }
        out_printf(req, "%s{\"name\":", first ? "" : ",");
        out_json_string(req, info.fname);
        out_printf(req, ",\"dir\":%s,\"size\":%llu,\"date\":%u,\"time\":%u}",
                   (info.fattrib & AM_DIR) ? "true" : "false", (unsigned long long)info.fsize,
                   (unsigned)info.fdate, (unsigned)info.ftime);
        first = false;
        storage_touch();
    }
    f_closedir(&dir);
    // "complete" lets the page tell a full listing from one cut short by an error.
    out_printf(req, "],\"complete\":%s}", complete ? "true" : "false");
    out_flush(req);
}

static void handle_download(request_t *req) {
    if (!query_path(req, "path", path_a, sizeof(path_a)) || !require_storage(req)) {
        return;
    }
    static FIL file;
    FRESULT fr = f_open(&file, path_a, FA_READ);
    if (fr != FR_OK) {
        reply_fresult(req, fr);
        return;
    }
    char extra[48];
    snprintf(extra, sizeof(extra), "Content-Length: %llu\r\n", (unsigned long long)f_size(&file));
    out_head(req, 200, "application/octet-stream", extra);
    out_flush(req);

    uint64_t sent = 0;
    while (out_ok) {
        UINT got = 0;
        fr = f_read(&file, io_buf, IO_SIZE, &got);
        if (fr != FR_OK || got == 0) {
            break;
        }
        storage_touch();
        if (!send_all(req->conn, io_buf, got)) {
            break;
        }
        sent += got;
    }
    if (sent != f_size(&file)) {
        // The declared length can no longer be honoured; closing early makes
        // the browser report a failed download rather than a truncated file.
        log_printf("http: download of %s aborted at %llu bytes (fr=%d)", path_a, (unsigned long long)sent, (int)fr);
    }
    f_close(&file);
}

static void handle_upload(request_t *req) {
    if (!query_path(req, "path", path_a, sizeof(path_a))) {
        return;
    }
    if (!req->has_length) {
        reply_error(req, 411, NULL, "Content-Length is required.");
        return;
    }
    if (strlen(path_a) < 2) {
        reply_error(req, 400, NULL, "Missing file name.");
        return;
    }
    if (!require_storage(req)) {
        return;
    }

    // Upload under a temporary name so a failed transfer never destroys an
    // existing file of the same name.
    snprintf(path_b, sizeof(path_b), "%s.part", path_a);
    static FIL file;
    FRESULT fr = f_open(&file, path_b, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        reply_fresult(req, fr);
        return;
    }

    bool net_failed = false;
    bool disk_full = false;
    uint64_t total = 0;
    while (total < req->content_length) {
        // Fill the whole buffer before writing so FatFs can pass large,
        // sector aligned runs straight through to the card.
        size_t fill = 0;
        while (fill < IO_SIZE && (total + fill) < req->content_length) {
            size_t n = body_read(req, io_buf + fill, IO_SIZE - fill);
            if (n == 0) {
                net_failed = true;
                break;
            }
            fill += n;
            storage_touch();
        }
        if (net_failed) {
            break;
        }
        UINT written = 0;
        fr = f_write(&file, io_buf, (UINT)fill, &written);
        storage_touch();
        if (fr != FR_OK) {
            break;
        }
        if (written != fill) {
            disk_full = true;
            break;
        }
        total += fill;
    }

    FRESULT close_fr = f_close(&file);
    if (fr == FR_OK) {
        fr = close_fr;
    }
    if (net_failed || disk_full || fr != FR_OK) {
        f_unlink(path_b);
        log_printf("http: upload of %s failed at %llu bytes (net=%d full=%d fr=%d)", path_a,
                   (unsigned long long)total, net_failed, disk_full, (int)fr);
        if (net_failed) {
            return;   // nobody left to tell
        }
        if (disk_full) {
            reply_error(req, 507, NULL, "The SD card is full.");
        } else {
            reply_fresult(req, fr);
        }
        return;
    }

    fr = f_unlink(path_a);
    if (fr == FR_OK || fr == FR_NO_FILE) {
        fr = f_rename(path_b, path_a);
    }
    if (fr != FR_OK) {
        f_unlink(path_b);
        if (fr == FR_DENIED) {
            reply_error(req, 409, NULL, "A folder or read-only file with that name already exists.");
            return;
        }
    }
    reply_fresult(req, fr);
}

static void handle_mkdir(request_t *req) {
    if (!query_path(req, "path", path_a, sizeof(path_a)) || !require_storage(req)) {
        return;
    }
    reply_fresult(req, f_mkdir(path_a));
}

// Delete a file or a whole directory tree. path is modified in place while
// descending and restored on return.
static FRESULT remove_tree(char *path, size_t cap, int depth) {
    static FILINFO info;
    FRESULT fr = f_unlink(path);
    if (fr != FR_DENIED) {
        return fr;
    }
    // FR_DENIED: either a non-empty directory or a read-only entry.
    if (depth >= MAX_DEPTH) {
        return FR_DENIED;
    }
    DIR dir;
    if (f_opendir(&dir, path) != FR_OK) {
        return FR_DENIED;
    }
    size_t len = strlen(path);
    for (;;) {
        fr = f_readdir(&dir, &info);
        if (fr != FR_OK || info.fname[0] == '\0') {
            break;
        }
        if ((len + 1 + strlen(info.fname) + 1) > cap) {
            fr = FR_INVALID_NAME;
            break;
        }
        path[len] = '/';
        strcpy(path + len + 1, info.fname);
        fr = remove_tree(path, cap, depth + 1);
        path[len] = '\0';
        if (fr != FR_OK) {
            break;
        }
        storage_touch();
        plat_poll();
    }
    f_closedir(&dir);
    return (fr == FR_OK) ? f_unlink(path) : fr;
}

static void handle_delete(request_t *req) {
    if (!query_path(req, "path", path_a, sizeof(path_a))) {
        return;
    }
    if (strlen(path_a) < 2) {
        reply_error(req, 400, NULL, "Refusing to delete the root folder.");
        return;
    }
    if (!require_storage(req)) {
        return;
    }
    reply_fresult(req, remove_tree(path_a, sizeof(path_a), 0));
}

static void handle_rename(request_t *req) {
    if (!query_path(req, "from", path_a, sizeof(path_a)) ||
        !query_path(req, "to", path_b, PATH_MAX_LEN)) {
        return;
    }
    if (strlen(path_a) < 2 || strlen(path_b) < 2) {
        reply_error(req, 400, NULL, "Missing name.");
        return;
    }
    if (!require_storage(req)) {
        return;
    }
    reply_fresult(req, f_rename(path_a, path_b));
}

static void handle_time(request_t *req) {
    // t=YYYYMMDDhhmmss (local time as the browser sees it), wd=1..7 (1 = Monday)
    char t[16], wd[4];
    if (!query_get(req, "t", t, sizeof(t)) || strlen(t) != 14 || strspn(t, "0123456789") != 14 ||
        !query_get(req, "wd", wd, sizeof(wd)) || wd[0] < '1' || wd[0] > '7') {
        reply_error(req, 400, NULL, "Invalid time.");
        return;
    }
#define DIGITS2(p) ((uint8_t)(((p)[0] - '0') * 10 + ((p)[1] - '0')))
    sc64_time_t time = {
        .weekday = (uint8_t)(wd[0] - '0'),
        .year = (uint16_t)(DIGITS2(t) * 100 + DIGITS2(t + 2)),
        .month = DIGITS2(t + 4),
        .day = DIGITS2(t + 6),
        .hour = DIGITS2(t + 8),
        .minute = DIGITS2(t + 10),
        .second = DIGITS2(t + 12),
    };
#undef DIGITS2
    if (time.year < 1980 || time.year > 2099 || time.month < 1 || time.month > 12 || time.day < 1 ||
        time.day > 31 || time.hour > 23 || time.minute > 59 || time.second > 59) {
        reply_error(req, 400, NULL, "Invalid time.");
        return;
    }
    if (!sc64_time_set(&time)) {
        reply_error(req, 503, NULL, "SummerCart64 is not responding.");
        return;
    }
    reply_ok(req);
}

static void handle_restart(request_t *req) {
    storage_release();
    reply_ok(req);
    plat_restart();
}

// ---- firmware self-update ----

#define UF2_BLOCK_SIZE 512
#define UF2_PAYLOAD_SIZE 256
#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END 0x0AB16F30u
#define UF2_FLAG_NOT_MAIN_FLASH 0x00000001u
#define UF2_FLAG_FAMILY_ID_PRESENT 0x00002000u
#define FLASH_BASE 0x10000000u

// Present in every build of this firmware; an uploaded image without it is
// some other program and is refused.
static const char firmware_marker[] = "SC64-WEBUI-FIRMWARE";

static uint32_t update_sectors;   // non-zero: staged image waiting to be applied

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Write one sector to the staging area (NULL data: leave it blank).
static bool stage_sector(uint32_t sector, const uint8_t *data) {
    if (!plat_fw_stage(sector, data)) {
        log_printf("update: flash write failed at sector %lu", (unsigned long)sector);
        return false;
    }
    plat_poll();   // flash writes run with interrupts off; let the network catch up
    return true;
}

// Receive a .uf2 and write its contents to the staging area. Returns NULL
// on success (with *sectors set) or an error message.
static const char *stage_uf2(request_t *req, uint32_t *sectors) {
    static uint8_t sector_data[FW_SECTOR_SIZE];
    uint8_t block[UF2_BLOCK_SIZE];
    uint32_t current = UINT32_MAX;   // sector being assembled in sector_data
    uint32_t next_address = FLASH_BASE;
    uint32_t blocks = 0, expected_blocks = 0;

    if (req->content_length == 0 || (req->content_length % UF2_BLOCK_SIZE) != 0) {
        return "That is not a .uf2 firmware file.";
    }
    while (req->body_read < req->content_length) {
        for (size_t fill = 0; fill < UF2_BLOCK_SIZE; ) {
            size_t n = body_read(req, block + fill, UF2_BLOCK_SIZE - fill);
            if (n == 0) {
                log_printf("update: upload stopped after %llu bytes", (unsigned long long)req->body_read);
                return "Connection lost during upload.";
            }
            fill += n;
        }
        if (get_le32(block) != UF2_MAGIC_START0 || get_le32(block + 4) != UF2_MAGIC_START1 ||
            get_le32(block + 508) != UF2_MAGIC_END) {
            return "That is not a .uf2 firmware file.";
        }
        uint32_t flags = get_le32(block + 8), address = get_le32(block + 12), size = get_le32(block + 16);
        uint32_t total = get_le32(block + 24), family = get_le32(block + 28);
        if ((flags & UF2_FLAG_NOT_MAIN_FLASH) || !(flags & UF2_FLAG_FAMILY_ID_PRESENT) ||
            family != plat_fw_family()) {
            continue;   // not for this chip (RP2350 files also start with a marker block to skip)
        }
        if (size != UF2_PAYLOAD_SIZE || (address % UF2_PAYLOAD_SIZE) != 0 || address < next_address ||
            (address - FLASH_BASE + size) > plat_fw_capacity()) {
            return "The firmware file has an unexpected layout.";
        }
        uint32_t sector = (address - FLASH_BASE) / FW_SECTOR_SIZE;
        if (sector != current) {
            if (current != UINT32_MAX && !stage_sector(current, sector_data)) {
                return "Writing the update to flash failed.";
            }
            // Sectors the file skips must not keep stale data.
            for (uint32_t gap = (current == UINT32_MAX) ? 0 : current + 1; gap < sector; gap++) {
                if (!stage_sector(gap, NULL)) {
                    return "Writing the update to flash failed.";
                }
            }
            memset(sector_data, 0xFF, sizeof(sector_data));
            current = sector;
        }
        memcpy(sector_data + ((address - FLASH_BASE) % FW_SECTOR_SIZE), block + 32, UF2_PAYLOAD_SIZE);
        next_address = address + size;
        expected_blocks = total;
        blocks++;
    }
    if (blocks == 0) {
        return "That firmware file is for a different board.";
    }
    if (blocks != expected_blocks) {
        return "The firmware file is incomplete.";
    }
    if (!stage_sector(current, sector_data)) {
        return "Writing the update to flash failed.";
    }

    const uint8_t *image = plat_fw_staged();
    size_t length = (size_t)(current + 1) * FW_SECTOR_SIZE, marker_len = sizeof(firmware_marker) - 1;
    bool found = false;
    for (size_t i = 0; !found && (i + marker_len) <= length; i++) {
        found = (image[i] == (uint8_t)firmware_marker[0] && memcmp(image + i, firmware_marker, marker_len) == 0);
    }
    if (!found) {
        return "That file is not SummerCart64 Web UI firmware.";
    }
    *sectors = current + 1;
    return NULL;
}

static void handle_firmware(request_t *req) {
    log_printf("update: receiving %llu bytes", (unsigned long long)req->content_length);
    uint32_t sectors = 0;
    const char *error = stage_uf2(req, &sectors);
    if (error) {
        log_printf("update: rejected (%s)", error);
        reply_error(req, 400, NULL, error);
        return;
    }
    log_printf("update: staged %lu kB, installing after this reply", (unsigned long)(sectors * FW_SECTOR_SIZE / 1024));
    reply_ok(req);
    update_sectors = sectors;   // applied by http_task() once this reply is on its way
}

typedef struct {
    const char *method;
    const char *path;
    void (*handler)(request_t *req);
} route_t;

static const route_t routes[] = {
    {"GET", "/", handle_index},
    {"GET", "/api/status", handle_status},
    {"GET", "/api/log", handle_log},
    {"GET", "/api/df", handle_df},
    {"GET", "/api/list", handle_list},
    {"GET", "/api/download", handle_download},
    {"PUT", "/api/upload", handle_upload},
    {"POST", "/api/mkdir", handle_mkdir},
    {"POST", "/api/delete", handle_delete},
    {"POST", "/api/rename", handle_rename},
    {"POST", "/api/time", handle_time},
    {"POST", "/api/restart", handle_restart},
    {"PUT", "/api/firmware", handle_firmware},
};

static void serve(net_conn_t *conn) {
    // Read the request header.
    size_t len = 0;
    char *end = NULL;
    while (!end) {
        if (len >= HEADER_MAX) {
            return;
        }
        size_t n = recv_some(conn, (uint8_t *)header_buf + len, HEADER_MAX - len);
        if (n == 0) {
            return;
        }
        len += n;
        header_buf[len] = '\0';
        end = strstr(header_buf, "\r\n\r\n");
    }
    end[2] = '\0';

    request_t req = {.conn = conn};
    req.body = (uint8_t *)end + 4;
    req.body_len = len - (size_t)((end + 4) - header_buf);

    // Request line: METHOD SP target SP version
    char *line_end = strstr(header_buf, "\r\n");
    char *headers = line_end + 2;
    *line_end = '\0';
    char *sp1 = strchr(header_buf, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || (size_t)(sp1 - header_buf) >= sizeof(req.method)) {
        return;
    }
    *sp1 = '\0';
    *sp2 = '\0';
    strcpy(req.method, header_buf);
    req.path = sp1 + 1;
    req.query = strchr(req.path, '?');
    if (req.query) {
        *req.query++ = '\0';
    } else {
        req.query = sp2;   // empty string
    }

    const char *value = header_value(headers, "Content-Length");
    if (value) {
        req.content_length = strtoull(value, NULL, 10);
        req.has_length = true;
    }
    if (req.body_len > req.content_length) {
        req.body_len = (size_t)req.content_length;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (strcmp(req.method, routes[i].method) == 0 && strcmp(req.path, routes[i].path) == 0) {
            routes[i].handler(&req);
            return;
        }
    }
    reply_error(&req, 404, NULL, "Not found.");
}

bool http_init(uint16_t port) {
    return net_listen(port);
}

void http_task(void) {
    net_conn_t *conn = net_next_ready();
    if (conn) {
        serve(conn);
        net_close(conn);
    }
    if (update_sectors) {
        // Give the reply time to reach the browser, then hand over.
        uint32_t start = plat_millis();
        while ((plat_millis() - start) < 1500) {
            plat_poll();
        }
        storage_release();
        plat_fw_apply(update_sectors);
        update_sectors = 0;
    }
}
