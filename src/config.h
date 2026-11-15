// Wi-Fi settings stored on the SD card.
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

// Lives inside N64FlashcartMenu's own folder, which the menu hides from its
// file browser. Plain text, one "key=value" per line:
//   ssid=My Network
//   password=secret
#define WIFI_CONFIG_PATH "/menu/webui/wifi.txt"

typedef struct {
    char ssid[33];
    char password[65];
} wifi_config_t;

// Read the settings file. The card must be mounted (storage_acquire()).
// False if the file is missing or has no usable ssid.
bool wifi_config_read(wifi_config_t *config);

#endif
