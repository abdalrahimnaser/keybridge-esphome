#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYBRIDGE_HID_PAGE_KEYBOARD 0x07
#define KEYBRIDGE_HID_PAGE_CONSUMER 0x0C

// pressed=1 down, 0 up. page=0 usage=0 pressed=0 means release every key.
typedef void (*keybridge_hid_event_cb_t)(uint16_t usage_page, uint16_t usage, uint8_t pressed);

void keybridge_usb_start(void);
void keybridge_set_hid_callback(keybridge_hid_event_cb_t cb);

#ifdef __cplusplus
}
#endif
