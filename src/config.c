#include <string.h>

#include "ff.h"

#include "config.h"

static void copy_value(char *out, size_t cap, const char *value, size_t len) {
    if (len < cap) {
        memcpy(out, value, len);
        out[len] = '\0';
    }
}

bool wifi_config_read(wifi_config_t *config) {
    static FIL file;
    char text[512];
    UINT len = 0;

    memset(config, 0, sizeof(*config));
    if (f_open(&file, WIFI_CONFIG_PATH, FA_READ) != FR_OK) {
        return false;
    }
    FRESULT fr = f_read(&file, text, sizeof(text) - 1, &len);
    f_close(&file);
    if (fr != FR_OK) {
        return false;
    }
    text[len] = '\0';

    char *line = text;
    if (strncmp(line, "\xEF\xBB\xBF", 3) == 0) {
        line += 3;   // UTF-8 byte order mark, as written by Windows Notepad
    }
    while (*line) {
        size_t line_len = strcspn(line, "\r\n");
        char *next = line + line_len;
        next += strspn(next, "\r\n");
        // The value is everything after '=' up to the end of the line, so
        // passwords may contain spaces and '=' themselves.
        if (strncmp(line, "ssid=", 5) == 0) {
            copy_value(config->ssid, sizeof(config->ssid), line + 5, line_len - 5);
        } else if (strncmp(line, "password=", 9) == 0) {
            copy_value(config->password, sizeof(config->password), line + 9, line_len - 9);
        }
        line = next;
    }
    return config->ssid[0] != '\0';
}
