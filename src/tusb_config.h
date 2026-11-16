#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// The Pico's native USB port runs as a full-speed host for the SC64's FT232H.
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB 0
#define CFG_TUH_DEVICE_MAX 1

#define CFG_TUH_CDC 1
#define CFG_TUH_CDC_FTDI 1
// FT232H latency timer in ms: how long short responses sit in the chip.
#define CFG_TUH_CDC_FTDI_LATENCY 1
// DTR is the SC64's protocol reset line, so leave it low on enumeration
// (CFG_TUH_CDC_LINE_CONTROL_ON_ENUM deliberately not defined).

// Every FTDI IN packet starts with two status bytes, so receive transfers
// must be exactly one packet long.
#define CFG_TUH_CDC_RX_EPSIZE 64
#define CFG_TUH_CDC_RX_BUFSIZE 4096
// Data is sent with tuh_edpt_xfer() rather than the driver's transmit stream
// (see transport_tusb.c), so that stream is kept as small as possible.
#define CFG_TUH_API_EDPT_XFER 1
#define CFG_TUH_CDC_TX_EPSIZE 64
#define CFG_TUH_CDC_TX_BUFSIZE 64

#endif
