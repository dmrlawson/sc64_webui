// SummerCart64 Web UI - Raspberry Pi Pico W / Pico 2 W firmware entry point.
//
// The native USB port is a USB host talking to the SC64; the CYW43 radio
// serves the web page. Everything runs cooperatively from one main loop.
//
// Wi-Fi settings come from a file on the SC64's SD card (see config.h).
// With settings: join that network and keep retrying if it is unavailable.
// Without: start an access point so the file can be put there via the page.

#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/apps/mdns.h"
#include "tusb.h"

#include "config.h"
#include "dhcpserver.h"
#include "http.h"
#include "platform.h"
#include "storage.h"

#define HOSTNAME CYW43_HOST_NAME   // set in CMakeLists.txt, also used for DHCP
#define AP_SSID "SC64-WebUI"
#define AP_PASSWORD "summercart64"

#define CONFIG_WAIT_MS 10000       // how long to wait at boot for the SC64 to show up
#define CONNECT_TIMEOUT_MS 20000
#define RECONNECT_INTERVAL_MS 10000
#define SETUP_CHECK_INTERVAL_MS 2000

static wifi_config_t config;
static bool ap_mode;
static bool config_checked;   // the card has been readable at least once
static bool rebooting;
static dhcp_server_t dhcp_server;

uint32_t plat_millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

void plat_poll(void) {
    tuh_task();
    cyw43_arch_poll();
}

static uint32_t wifi_auth(void) {
    return config.password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
}

static void start_mdns(struct netif *netif) {
    mdns_resp_init();
    mdns_resp_add_netif(netif, HOSTNAME);
    mdns_resp_add_service(netif, "SummerCart64 Web UI", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
}

// Look for the settings file, waiting a while for the SC64 to enumerate.
static bool load_config(void) {
    bool found = false;
    uint32_t last_try = 0;
    while (plat_millis() < CONFIG_WAIT_MS) {
        plat_poll();
        if ((plat_millis() - last_try) < 250) {
            continue;
        }
        last_try = plat_millis();
        storage_state_t state = storage_acquire();
        if (state == STORAGE_READY) {
            config_checked = true;
            found = wifi_config_read(&config);
            storage_release();
            break;
        }
        if (state != STORAGE_NO_USB && state != STORAGE_NO_LINK) {
            log_printf("wifi: cannot read settings yet (%s)", storage_state_name(state));
            break;   // console on, no card...: waiting longer will not help
        }
    }
    return found;
}

static void start_station(void) {
    cyw43_arch_enable_sta_mode();
    // Power saving adds ~100 ms of latency to every request.
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);
    for (int attempt = 1;; attempt++) {
        log_printf("wifi: connecting to \"%s\" (attempt %d)", config.ssid, attempt);
        int err = cyw43_arch_wifi_connect_timeout_ms(config.ssid, config.password, wifi_auth(), CONNECT_TIMEOUT_MS);
        if (err == 0) {
            break;
        }
        log_printf("wifi: connect failed (%d)", err);
    }
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
    log_printf("wifi: connected, http://%s/ or http://%s.local/", ip4addr_ntoa(netif_ip4_addr(netif)), HOSTNAME);
    start_mdns(netif);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
}

static void start_access_point(void) {
    ap_mode = true;
    cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    struct netif *netif = &cyw43_state.netif[CYW43_ITF_AP];
    ip4_addr_t gateway = *netif_ip4_addr(netif), mask = *netif_ip4_netmask(netif);
    dhcp_server_init(&dhcp_server, &gateway, &mask);
    start_mdns(netif);
    log_printf("wifi: no settings read from %s, access point \"%s\" password \"%s\", http://%s/", WIFI_CONFIG_PATH,
               AP_SSID, AP_PASSWORD, ip4addr_ntoa(&gateway));
}

// Access point mode: restart as soon as a settings file turns up, either
// because the card became readable (console switched off) or because the
// file was just uploaded through the page.
static void setup_task(void) {
    static uint32_t last_check, last_blink;
    static bool led;
    uint32_t now = plat_millis();

    if ((now - last_blink) > 500) {
        last_blink = now;
        led = !led;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
    }
    if (rebooting || (now - last_check) < SETUP_CHECK_INTERVAL_MS) {
        return;
    }
    last_check = now;

    // Piggyback on the card already being mounted for the page; only mount
    // it ourselves until we have had one look.
    bool was_mounted = storage_mounted();
    if (!was_mounted && (config_checked || storage_acquire() != STORAGE_READY)) {
        return;
    }
    config_checked = true;
    bool found = wifi_config_read(&config);
    if (found || !was_mounted) {
        storage_release();
    }
    if (found) {
        log_printf("wifi: settings found, restarting");
        rebooting = true;
        watchdog_reboot(0, 0, 500);   // leaves time to finish answering the page
    }
}

static void station_task(void) {
    static uint32_t last_check;
    uint32_t now = plat_millis();
    if ((now - last_check) < RECONNECT_INTERVAL_MS) {
        return;
    }
    last_check = now;
    bool up = (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, up);
    if (!up) {
        log_printf("wifi: link down, reconnecting");
        cyw43_arch_wifi_connect_async(config.ssid, config.password, wifi_auth());
    }
}

// The SDK's default stack lives in a 4 kiB scratch region, which is too small
// for FatFs + lwIP + TinyUSB nested in one loop, so run on our own stack.
static uint8_t app_stack[32 * 1024] __attribute__((aligned(8)));

static void app_main(void) __attribute__((noreturn, used));

int main(void) {
    __asm volatile(
#if PICO_RP2350
        "msr msplim, %0\n"   // stack overflow guard (not available on the RP2040)
#endif
        "mov sp, %1\n"
        "bl app_main\n"
        : : "r"(app_stack), "r"(app_stack + sizeof(app_stack)));
    __builtin_unreachable();
}

static void app_main(void) {
    stdio_init_all();
    log_printf("SummerCart64 Web UI %s starting", WEBUI_VERSION);

    if (cyw43_arch_init() != 0) {
        panic("cyw43 init failed");
    }
    const tusb_rhport_init_t usb_init = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &usb_init);

    if (load_config()) {
        start_station();
    } else {
        start_access_point();
    }
    if (!http_init(80)) {
        log_printf("http: could not start server");
    }

    for (;;) {
        plat_poll();
        storage_task();
        if (ap_mode) {
            setup_task();
        } else {
            station_task();
        }
        http_task();
    }
}
