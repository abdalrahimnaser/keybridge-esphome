#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "usb/usb_types_stack.h"

#include "keybridge_usb_driver.h"

static const char *TAG = "KeyBridge HID";

#define HID_MAX_IF 3
#define HID_MAX_FIELDS 96
#define HID_MAX_HELD 16
#define HID_DESC_MAX 1024
#define HID_IN_RETRY_MAX 3
#define HID_CONSUMER_IDLE_US 150000

#define HID_DESC_TYPE_HID 0x21
#define HID_DESC_TYPE_REPORT 0x22
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_PROTOCOL_REPORT 1

#define WORK_SETUP 1
#define WORK_TEARDOWN 2

#define FLD_ARRAY (1u << 0)
#define FLD_VARIABLE (1u << 1)
#define FLD_RELATIVE (1u << 2)
#define FLD_CONSTANT (1u << 3)

typedef struct {
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint8_t count;
    uint8_t flags;
    uint16_t usage_page;
    uint16_t usage;
    uint16_t usage_min;
    uint16_t usage_max;
} hid_field_t;

typedef struct {
    uint8_t if_number;
    uint8_t alt_setting;
    uint8_t ep_addr;
    uint16_t ep_mps;
    uint8_t protocol;
    bool claimed;
    bool uses_report_id;
    usb_transfer_t *in;
    int error_count;
    hid_field_t fields[HID_MAX_FIELDS];
    int nfields;
} hid_iface_t;

typedef struct {
    uint16_t page;
    uint16_t usage;
} hid_usage_t;

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static usb_transfer_t *s_ctrl;
static hid_iface_t s_ifaces[HID_MAX_IF];
static int s_nifaces;
static bool s_gone;
static bool s_busy_setup;
static int s_inflight;
static uint32_t s_held[HID_MAX_HELD];
static int s_nheld;

static SemaphoreHandle_t s_ctrl_done;
static SemaphoreHandle_t s_mu; // protects RAM variables about the device: s_dev, s_ifaces[], s_ctrl, s_inflight, s_held[], s_gone, s_busy_setup
static QueueHandle_t s_work_q;
static esp_timer_handle_t s_consumer_timer;
static keybridge_hid_event_cb_t s_cb;

static void teardown_locked(void);
static void decode_report(hid_iface_t *iface, const uint8_t *data, int len);

void keybridge_set_hid_callback(keybridge_hid_event_cb_t cb) {
    s_cb = cb;
}

static uint32_t usage_key(uint16_t page, uint16_t usage) {
    return ((uint32_t)page << 16) | usage;
}

static void emit(uint16_t page, uint16_t usage, uint8_t pressed) {
    if (page == 0 && usage == 0) {
        ESP_LOGI(TAG, "HID release-all");
    } else {
        ESP_LOGI(TAG, "HID %s %04X:%04X", pressed ? "down" : "up", page, usage);
    }
    if (s_cb) {
        s_cb(page, usage, pressed);
    }
}

static void release_all_held(void) {
    for (int i = 0; i < s_nheld; i++) {
        emit((uint16_t)(s_held[i] >> 16), (uint16_t)s_held[i], 0);
    }
    s_nheld = 0;
    emit(0, 0, 0);
}

static bool held_contains(const uint32_t *set, int n, uint32_t key) {
    for (int i = 0; i < n; i++) {
        if (set[i] == key) {
            return true;
        }
    }
    return false;
}

static void consumer_timeout(void *arg) {
    (void)arg;
    if (xSemaphoreTake(s_mu, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    uint32_t keep[HID_MAX_HELD];
    int nkeep = 0;
    for (int i = 0; i < s_nheld; i++) {
        uint16_t page = (uint16_t)(s_held[i] >> 16);
        if (page == KEYBRIDGE_HID_PAGE_CONSUMER) {
            emit(page, (uint16_t)s_held[i], 0);
        } else {
            keep[nkeep++] = s_held[i];
        }
    }
    memcpy(s_held, keep, (size_t)nkeep * sizeof(uint32_t));
    s_nheld = nkeep;
    xSemaphoreGive(s_mu);
}

static void bump_consumer_idle(void) {
    if (s_consumer_timer) {
        esp_timer_stop(s_consumer_timer);
        esp_timer_start_once(s_consumer_timer, HID_CONSUMER_IDLE_US);
    }
}

static void ctrl_done_cb(usb_transfer_t *xfer) {
    (void)xfer;
    if (s_ctrl_done) {
        xSemaphoreGive(s_ctrl_done);
    }
}

static bool ctrl_xfer(uint8_t bm, uint8_t req, uint16_t value, uint16_t index, uint16_t length,
                     uint8_t *out, int *out_len) {
    if (s_ctrl == NULL || s_dev == NULL || s_gone) {
        return false;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    setup->bmRequestType = bm;
    setup->bRequest = req;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;

    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->num_bytes = (int)sizeof(usb_setup_packet_t) + length;
    s_ctrl->timeout_ms = 500;
    s_ctrl->callback = ctrl_done_cb;

    xSemaphoreTake(s_ctrl_done, 0);
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) {
        ESP_LOGE(TAG, "control submit failed req=0x%02X", req);
        return false;
    }
    if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(600)) != pdTRUE) {
        ESP_LOGE(TAG, "control timeout req=0x%02X", req);
        return false;
    }
    if (s_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "control status=%d req=0x%02X", s_ctrl->status, req);
        return false;
    }
    if (out && length) {
        int n = s_ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (n < 0) {
            n = 0;
        }
        if (n > length) {
            n = length;
        }
        memcpy(out, s_ctrl->data_buffer + sizeof(usb_setup_packet_t), (size_t)n);
        if (out_len) {
            *out_len = n;
        }
    }
    return true;
}

static uint32_t hid_item_data(const uint8_t *p, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) {
        v |= ((uint32_t)p[i]) << (8 * i);
    }
    return v;
}

static int32_t hid_item_signed(uint32_t raw, uint8_t size) {
    if (size == 1) {
        return (int8_t)raw;
    }
    if (size == 2) {
        return (int16_t)raw;
    }
    return (int32_t)raw;
}

static bool page_interesting(uint16_t page) {
    return page == KEYBRIDGE_HID_PAGE_KEYBOARD || page == KEYBRIDGE_HID_PAGE_CONSUMER ||
           page == 0x09 || page == 0x01;
}

static bool skip_desktop_usage(uint16_t page, uint16_t usage) {
    if (page != 0x01) {
        return false;
    }
    // Keep system power / sleep / wake. Drop axes, hats, pointer.
    return usage < 0x80 || usage > 0x83;
}

static void add_field(hid_iface_t *iface, hid_field_t f) {
    if (f.flags & (FLD_CONSTANT | FLD_RELATIVE)) {
        return;
    }
    if (!page_interesting(f.usage_page)) {
        return;
    }
    if (f.usage && skip_desktop_usage(f.usage_page, f.usage)) {
        return;
    }
    if (f.usage_min && skip_desktop_usage(f.usage_page, f.usage_min) && f.usage == 0) {
        return;
    }
    if (iface->nfields >= HID_MAX_FIELDS) {
        return;
    }
    if (f.report_id) {
        iface->uses_report_id = true;
    }
    iface->fields[iface->nfields++] = f;
}

static void parse_report_descriptor(hid_iface_t *iface, const uint8_t *desc, int desc_len) {
    iface->nfields = 0;
    iface->uses_report_id = false;

    uint16_t usage_page = 0;
    uint8_t report_id = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    int32_t logical_max = 1;
    uint16_t bitpos[256];
    memset(bitpos, 0, sizeof(bitpos));

    uint32_t locals[16];
    int nlocal = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    bool has_range = false;

    uint16_t col_page[8];
    uint16_t col_usage[8];
    int col_depth = 0;

    uint16_t saved_page = 0;
    uint32_t saved_size = 0;
    uint32_t saved_count = 0;
    uint8_t saved_rid = 0;
    int32_t saved_lmax = 1;
    bool have_push = false;

    int i = 0;
    while (i < desc_len) {
        uint8_t prefix = desc[i++];
        if (prefix == 0xFE) {
            if (i + 2 > desc_len) {
                break;
            }
            uint8_t long_size = desc[i];
            i += 2 + long_size;
            continue;
        }

        uint8_t size_code = prefix & 0x03;
        uint8_t size = (size_code == 3) ? 4 : size_code;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = prefix >> 4;
        if (i + size > desc_len) {
            break;
        }
        uint32_t data = hid_item_data(desc + i, size);
        i += size;

        if (type == 1) {  // global
            switch (tag) {
                case 0x0:
                    usage_page = (uint16_t)data;
                    break;
                case 0x2:
                    logical_max = hid_item_signed(data, size);
                    break;
                case 0x7:
                    report_size = data;
                    break;
                case 0x8:
                    report_id = (uint8_t)data;
                    break;
                case 0x9:
                    report_count = data;
                    break;
                case 0xA:
                    saved_page = usage_page;
                    saved_size = report_size;
                    saved_count = report_count;
                    saved_rid = report_id;
                    saved_lmax = logical_max;
                    have_push = true;
                    break;
                case 0xB:
                    if (have_push) {
                        usage_page = saved_page;
                        report_size = saved_size;
                        report_count = saved_count;
                        report_id = saved_rid;
                        logical_max = saved_lmax;
                        have_push = false;
                    }
                    break;
                default:
                    break;
            }
        } else if (type == 2) {  // local
            if (tag == 0x0) {
                uint16_t page = usage_page;
                uint16_t usage = (uint16_t)data;
                if (size == 4) {
                    usage = (uint16_t)(data & 0xFFFF);
                    page = (uint16_t)(data >> 16);
                }
                if (nlocal < 16) {
                    locals[nlocal++] = usage_key(page, usage);
                }
            } else if (tag == 0x1) {
                usage_min = data;
                has_range = true;
            } else if (tag == 0x2) {
                usage_max = data;
                has_range = true;
            }
        } else if (type == 0) {  // main
            bool in_mouse = false;
            for (int c = 0; c < col_depth; c++) {
                if (col_page[c] == 0x01 && (col_usage[c] == 0x01 || col_usage[c] == 0x02)) {
                    in_mouse = true;
                }
            }

            if (tag == 0xA) {  // collection
                if (col_depth < 8) {
                    uint16_t u = 0;
                    uint16_t p = usage_page;
                    if (nlocal) {
                        p = (uint16_t)(locals[0] >> 16);
                        u = (uint16_t)locals[0];
                    }
                    col_page[col_depth] = p;
                    col_usage[col_depth] = u;
                    col_depth++;
                }
            } else if (tag == 0xC) {
                if (col_depth > 0) {
                    col_depth--;
                }
            } else if (tag == 0x8) {  // input
                uint8_t flags = 0;
                if (data & 0x01) {
                    flags |= FLD_CONSTANT;
                }
                if (data & 0x02) {
                    flags |= FLD_VARIABLE;
                } else {
                    flags |= FLD_ARRAY;
                }
                if (data & 0x04) {
                    flags |= FLD_RELATIVE;
                }

                uint16_t bits = (uint16_t)(report_size * report_count);
                uint16_t off = bitpos[report_id];
                bitpos[report_id] = (uint16_t)(off + bits);

                bool skip = in_mouse || (flags & FLD_CONSTANT) || (flags & FLD_RELATIVE);
                if (!skip && (flags & FLD_VARIABLE) && report_size > 1 && logical_max > 1) {
                    skip = true;  // analog axis
                }

                if (!skip && report_size > 0 && report_count > 0 && report_count < 256 &&
                    report_size <= 32) {
                    hid_field_t f = {0};
                    f.report_id = report_id;
                    f.bit_offset = off;
                    f.bit_size = (uint8_t)report_size;
                    f.count = (uint8_t)report_count;
                    f.flags = flags;
                    f.usage_page = usage_page;

                    if (nlocal == 1) {
                        f.usage_page = (uint16_t)(locals[0] >> 16);
                        f.usage = (uint16_t)locals[0];
                    } else if (nlocal > 1 && (int)report_count == nlocal) {
                        for (int u = 0; u < nlocal; u++) {
                            hid_field_t one = f;
                            one.count = 1;
                            one.bit_offset = (uint16_t)(off + u * (int)report_size);
                            one.usage_page = (uint16_t)(locals[u] >> 16);
                            one.usage = (uint16_t)locals[u];
                            add_field(iface, one);
                        }
                        nlocal = 0;
                        has_range = false;
                        usage_min = usage_max = 0;
                        continue;
                    } else if (has_range) {
                        f.usage_min = (uint16_t)usage_min;
                        f.usage_max = (uint16_t)usage_max;
                    }
                    add_field(iface, f);
                }
            }

            nlocal = 0;
            has_range = false;
            usage_min = usage_max = 0;
        }
    }

    if (iface->nfields == 0 && iface->protocol == 0x01) {
        hid_field_t mods = {
            .report_id = 0,
            .bit_offset = 0,
            .bit_size = 1,
            .count = 8,
            .flags = FLD_VARIABLE,
            .usage_page = KEYBRIDGE_HID_PAGE_KEYBOARD,
            .usage_min = 0xE0,
            .usage_max = 0xE7,
        };
        hid_field_t keys = {
            .report_id = 0,
            .bit_offset = 16,
            .bit_size = 8,
            .count = 6,
            .flags = FLD_ARRAY,
            .usage_page = KEYBRIDGE_HID_PAGE_KEYBOARD,
            .usage_min = 0x00,
            .usage_max = 0xE7,
        };
        add_field(iface, mods);
        add_field(iface, keys);
        ESP_LOGW(TAG, "IF %u: no parsed fields, using Boot keyboard layout", iface->if_number);
    }

    ESP_LOGI(TAG, "IF %u: %d input fields, report IDs %s", iface->if_number, iface->nfields,
             iface->uses_report_id ? "yes" : "no");
}

static uint32_t extract_bits(const uint8_t *data, int nbytes, int bit_off, int nbits) {
    if (nbits <= 0 || nbits > 32 || bit_off < 0) {
        return 0;
    }
    int end = bit_off + nbits;
    if (end > nbytes * 8) {
        return 0;
    }
    uint32_t val = 0;
    for (int b = 0; b < nbits; b++) {
        int pos = bit_off + b;
        if (data[pos / 8] & (1u << (pos % 8))) {
            val |= (1u << b);
        }
    }
    return val;
}

static void add_found(hid_usage_t *found, int *nfound, uint16_t page, uint16_t usage) {
    if (page == KEYBRIDGE_HID_PAGE_KEYBOARD && usage >= 0x01 && usage <= 0x03) {
        return;  // ErrorRollOver / POSTFail / Undefined
    }
    if (usage == 0 && page != 0x09) {
        return;
    }
    if (*nfound >= HID_MAX_HELD) {
        return;
    }
    for (int i = 0; i < *nfound; i++) {
        if (found[i].page == page && found[i].usage == usage) {
            return;
        }
    }
    found[*nfound].page = page;
    found[*nfound].usage = usage;
    (*nfound)++;
}

static void decode_report(hid_iface_t *iface, const uint8_t *data, int len) {
    if (len <= 0 || iface->nfields == 0) {
        return;
    }

    uint8_t rid = 0;
    const uint8_t *payload = data;
    int payload_len = len;
    if (iface->uses_report_id) {
        rid = data[0];
        payload = data + 1;
        payload_len = len - 1;
        if (payload_len < 0) {
            return;
        }
    } else if (len == 9 && iface->protocol == 0x01) {
        // Boot-layout fallback: some pads prefix a report ID without a parsed ID.
        payload = data + 1;
        payload_len = 8;
    }

    hid_usage_t found[HID_MAX_HELD];
    int nfound = 0;
    bool saw_consumer = false;

    for (int f = 0; f < iface->nfields; f++) {
        const hid_field_t *fld = &iface->fields[f];
        if (iface->uses_report_id && fld->report_id != rid) {
            continue;
        }
        for (int s = 0; s < fld->count; s++) {
            uint32_t val = extract_bits(payload, payload_len, fld->bit_offset + s * fld->bit_size,
                                        fld->bit_size);
            uint16_t page = fld->usage_page;
            uint16_t usage = 0;
            if (fld->flags & FLD_ARRAY) {
                usage = (uint16_t)val;
                if (usage == 0) {
                    continue;
                }
            } else {
                if (val == 0) {
                    continue;
                }
                if (fld->usage) {
                    usage = fld->usage;
                } else {
                    usage = (uint16_t)(fld->usage_min + s);
                    if (fld->usage_max && usage > fld->usage_max) {
                        continue;
                    }
                }
            }
            if (page == KEYBRIDGE_HID_PAGE_CONSUMER) {
                saw_consumer = true;
            }
            add_found(found, &nfound, page, usage);
        }
    }

    uint32_t next[HID_MAX_HELD];
    int nnext = 0;

    // Keep held keys that this report is not responsible for (other report IDs / IFs).
    for (int i = 0; i < s_nheld; i++) {
        uint16_t page = (uint16_t)(s_held[i] >> 16);
        uint16_t usage = (uint16_t)s_held[i];
        bool owned = false;
        for (int f = 0; f < iface->nfields; f++) {
            const hid_field_t *fld = &iface->fields[f];
            if (iface->uses_report_id && fld->report_id != rid) {
                continue;
            }
            if (fld->usage_page != page) {
                continue;
            }
            if (fld->usage == usage || (usage >= fld->usage_min && usage <= fld->usage_max) ||
                (fld->flags & FLD_ARRAY)) {
                if (fld->flags & FLD_ARRAY) {
                    owned = (page == fld->usage_page);
                } else if (fld->usage) {
                    owned = (fld->usage == usage);
                } else {
                    owned = (usage >= fld->usage_min && usage <= fld->usage_max);
                }
                if (owned) {
                    break;
                }
            }
        }
        if (!owned) {
            if (nnext < HID_MAX_HELD) {
                next[nnext++] = s_held[i];
            }
        }
    }

    for (int i = 0; i < nfound; i++) {
        uint32_t key = usage_key(found[i].page, found[i].usage);
        if (!held_contains(next, nnext, key) && nnext < HID_MAX_HELD) {
            next[nnext++] = key;
        }
    }

    for (int i = 0; i < s_nheld; i++) {
        if (!held_contains(next, nnext, s_held[i])) {
            emit((uint16_t)(s_held[i] >> 16), (uint16_t)s_held[i], 0);
        }
    }
    for (int i = 0; i < nnext; i++) {
        if (!held_contains(s_held, s_nheld, next[i])) {
            emit((uint16_t)(next[i] >> 16), (uint16_t)next[i], 1);
        }
    }

    memcpy(s_held, next, (size_t)nnext * sizeof(uint32_t));
    s_nheld = nnext;

    if (saw_consumer) {
        bump_consumer_idle();
    }
}

static const usb_config_desc_t *active_config(void) {
    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s_dev, &cfg) != ESP_OK) {
        return NULL;
    }
    return cfg;
}

static uint16_t hid_report_desc_len(const usb_config_desc_t *cfg, uint8_t if_number, uint8_t alt) {
    const uint8_t *p = (const uint8_t *)cfg;
    int offset = 0;
    bool ours = false;
    while (offset + 2 <= cfg->wTotalLength) {
        uint8_t len = p[offset];
        uint8_t type = p[offset + 1];
        if (len < 2 || offset + len > cfg->wTotalLength) {
            break;
        }
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)(p + offset);
            ours = (intf->bInterfaceNumber == if_number && intf->bAlternateSetting == alt);
        } else if (ours && type == HID_DESC_TYPE_HID && len >= 9) {
            return (uint16_t)p[offset + 7] | ((uint16_t)p[offset + 8] << 8);
        }
        offset += len;
    }
    return 256;
}

static bool claim_hid_interfaces(void) {
    const usb_config_desc_t *cfg = active_config();
    if (!cfg) {
        ESP_LOGE(TAG, "no config descriptor");
        return false;
    }

    s_nifaces = 0;
    const uint8_t *p = (const uint8_t *)cfg;
    int offset = 0;
    const usb_intf_desc_t *cur = NULL;
    uint8_t cur_ep = 0;
    uint16_t cur_mps = 0;
    bool have_ep = false;

    while (offset + 2 <= cfg->wTotalLength) {
        uint8_t len = p[offset];
        uint8_t type = p[offset + 1];
        if (len < 2 || offset + len > cfg->wTotalLength) {
            break;
        }

        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && len >= 9) {
            if (cur && have_ep && s_nifaces < HID_MAX_IF) {
                hid_iface_t *dst = &s_ifaces[s_nifaces];
                memset(dst, 0, sizeof(*dst));
                dst->if_number = cur->bInterfaceNumber;
                dst->alt_setting = cur->bAlternateSetting;
                dst->ep_addr = cur_ep;
                dst->ep_mps = cur_mps;
                dst->protocol = cur->bInterfaceProtocol;
                s_nifaces++;
            }
            cur = (const usb_intf_desc_t *)(p + offset);
            have_ep = false;
            if (cur->bInterfaceClass != USB_CLASS_HID || cur->bInterfaceProtocol == 0x02) {
                cur = NULL;
            }
        } else if (cur && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && len >= 7) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)(p + offset);
            if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03 && !have_ep) {
                cur_ep = ep->bEndpointAddress;
                cur_mps = ep->wMaxPacketSize & 0x07FF;
                if (cur_mps == 0) {
                    cur_mps = 8;
                }
                have_ep = true;
            }
        }
        offset += len;
    }
    if (cur && have_ep && s_nifaces < HID_MAX_IF) {
        hid_iface_t *dst = &s_ifaces[s_nifaces];
        memset(dst, 0, sizeof(*dst));
        dst->if_number = cur->bInterfaceNumber;
        dst->alt_setting = cur->bAlternateSetting;
        dst->ep_addr = cur_ep;
        dst->ep_mps = cur_mps;
        dst->protocol = cur->bInterfaceProtocol;
        s_nifaces++;
    }

    if (s_nifaces == 0) {
        ESP_LOGW(TAG, "no HID input interfaces (mouse-only interfaces are skipped)");
        return false;
    }

    for (int n = 0; n < s_nifaces; n++) {
        hid_iface_t *iface = &s_ifaces[n];
        ESP_LOGI(TAG, "Claim HID IF %u alt %u proto %u EP 0x%02X MPS %u", iface->if_number,
                 iface->alt_setting, iface->protocol, iface->ep_addr, iface->ep_mps);
        if (usb_host_interface_claim(s_client, s_dev, iface->if_number, iface->alt_setting) !=
            ESP_OK) {
            ESP_LOGE(TAG, "claim IF %u failed", iface->if_number);
            return false;
        }
        iface->claimed = true;
    }
    return true;
}

static bool setup_interfaces(void) {
    static uint8_t desc[HID_DESC_MAX];
    const usb_config_desc_t *cfg = active_config();
    if (!cfg) {
        return false;
    }

    for (int n = 0; n < s_nifaces; n++) {
        hid_iface_t *iface = &s_ifaces[n];
        uint16_t want = hid_report_desc_len(cfg, iface->if_number, iface->alt_setting);
        if (want == 0 || want > HID_DESC_MAX) {
            want = HID_DESC_MAX;
        }

        int got = 0;
        bool ok = ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                                USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                            USB_B_REQUEST_GET_DESCRIPTOR, (HID_DESC_TYPE_REPORT << 8),
                            iface->if_number, want, desc, &got);
        if (ok && got > 0) {
            ESP_LOGI(TAG, "IF %u report descriptor %d bytes", iface->if_number, got);
            parse_report_descriptor(iface, desc, got);
        } else {
            ESP_LOGW(TAG, "IF %u GET_DESCRIPTOR(Report) failed", iface->if_number);
            parse_report_descriptor(iface, NULL, 0);
        }

        ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                  HID_SET_IDLE, 0, iface->if_number, 0, NULL, NULL);

        if (iface->protocol == 0x01) {
            // Prefer Report protocol so the descriptor we just parsed stays valid.
            ctrl_xfer(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                          USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                      HID_SET_PROTOCOL, HID_PROTOCOL_REPORT, iface->if_number, 0, NULL, NULL);
        }
    }
    return true;
}

static bool start_in_transfers(void) {
    for (int n = 0; n < s_nifaces; n++) {
        hid_iface_t *iface = &s_ifaces[n];
        if (usb_host_transfer_alloc(iface->ep_mps, 0, &iface->in) != ESP_OK) {
            ESP_LOGE(TAG, "IN alloc failed IF %u", iface->if_number);
            return false;
        }
        iface->in->device_handle = s_dev;
        iface->in->bEndpointAddress = iface->ep_addr;
        iface->in->num_bytes = iface->ep_mps;
        iface->in->callback = NULL;  // set below
        iface->in->context = iface;
        iface->error_count = 0;
    }
    return true;
}

static void in_complete_cb(usb_transfer_t *xfer);

static void arm_in_callbacks(void) {
    for (int n = 0; n < s_nifaces; n++) {
        s_ifaces[n].in->callback = in_complete_cb;
    }
}

static bool submit_in(hid_iface_t *iface) {
    if (s_gone || iface->in == NULL) {
        return false;
    }
    iface->in->num_bytes = iface->ep_mps;
    if (usb_host_transfer_submit(iface->in) != ESP_OK) {
        ESP_LOGE(TAG, "IN submit failed IF %u", iface->if_number);
        return false;
    }
    s_inflight++;
    return true;
}

static void in_complete_cb(usb_transfer_t *xfer) {
    hid_iface_t *iface = (hid_iface_t *)xfer->context;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_inflight--;

    if (s_gone || xfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        xfer->status == USB_TRANSFER_STATUS_CANCELED) {
        if (s_inflight <= 0 && !s_busy_setup) {
            uint8_t cmd = WORK_TEARDOWN;
            xQueueSend(s_work_q, &cmd, 0);
        }
        xSemaphoreGive(s_mu);
        return;
    }

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        decode_report(iface, xfer->data_buffer, xfer->actual_num_bytes);
        iface->error_count = 0;
        if (!submit_in(iface) && s_inflight <= 0 && !s_busy_setup) {
            uint8_t cmd = WORK_TEARDOWN;
            xQueueSend(s_work_q, &cmd, 0);
        }
        xSemaphoreGive(s_mu);
        return;
    }

    iface->error_count++;
    ESP_LOGE(TAG, "IN failed IF %u status=%d retry %d/%d", iface->if_number, xfer->status,
             iface->error_count, HID_IN_RETRY_MAX);
    if (iface->error_count < HID_IN_RETRY_MAX && submit_in(iface)) {
        xSemaphoreGive(s_mu);
        return;
    }
    if (s_inflight <= 0 && !s_busy_setup) {
        uint8_t cmd = WORK_TEARDOWN;
        xQueueSend(s_work_q, &cmd, 0);
    }
    xSemaphoreGive(s_mu);
}

static void teardown_locked(void) {
    if (s_consumer_timer) {
        esp_timer_stop(s_consumer_timer);
    }
    release_all_held();

    for (int n = 0; n < s_nifaces; n++) {
        hid_iface_t *iface = &s_ifaces[n];
        if (iface->in) {
            usb_host_transfer_free(iface->in);
            iface->in = NULL;
        }
        if (iface->claimed && s_dev && s_client) {
            usb_host_interface_release(s_client, s_dev, iface->if_number);
            iface->claimed = false;
        }
    }
    if (s_ctrl) {
        usb_host_transfer_free(s_ctrl);
        s_ctrl = NULL;
    }
    if (s_dev && s_client) {
        usb_host_device_close(s_client, s_dev);
    }
    s_dev = NULL;
    s_nifaces = 0;
    s_inflight = 0;
    s_gone = false;
    s_busy_setup = false;
    memset(s_ifaces, 0, sizeof(s_ifaces));
    ESP_LOGI(TAG, "device closed");
}

static void halt_endpoints(void) {
    if (!s_dev) {
        return;
    }
    for (int n = 0; n < s_nifaces; n++) {
        if (s_ifaces[n].ep_addr) {
            usb_host_endpoint_halt(s_dev, s_ifaces[n].ep_addr);
            usb_host_endpoint_flush(s_dev, s_ifaces[n].ep_addr);
        }
    }
}

static void worker_task(void *arg) {
    (void)arg;
    uint8_t cmd;
    while (xQueueReceive(s_work_q, &cmd, portMAX_DELAY) == pdTRUE) {
        if (cmd == WORK_TEARDOWN) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            teardown_locked();
            xSemaphoreGive(s_mu);
            continue;
        }
        if (cmd != WORK_SETUP) {
            continue; // if key is neither of these two (e.g junk) -> 
                    // wait for the next command
        }

        xSemaphoreTake(s_mu, portMAX_DELAY);

        // in case WORK_SETUP was called in a wrong place
        if (s_gone || s_dev == NULL) {
            teardown_locked();
            xSemaphoreGive(s_mu);
            continue;
        }
        
        s_busy_setup = true;
        xSemaphoreGive(s_mu);

        if (!setup_interfaces()) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            teardown_locked();
            xSemaphoreGive(s_mu);
            continue;
        }
        if (!start_in_transfers()) {
            xSemaphoreTake(s_mu, portMAX_DELAY);
            teardown_locked();
            xSemaphoreGive(s_mu);
            continue;
        }

        arm_in_callbacks();

        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_busy_setup = false;
        bool ok = !s_gone;
        for (int n = 0; ok && n < s_nifaces; n++) {
            if (!submit_in(&s_ifaces[n])) {
                ok = false;
            }
        }
        if (!ok) {
            teardown_locked();
        } else {
            ESP_LOGI(TAG, "listening on %d HID interface(s)", s_nifaces);
        }
        xSemaphoreGive(s_mu);
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(TAG, "device attached addr=%u", event_msg->new_dev.address);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (s_dev != NULL) {
            ESP_LOGW(TAG, "already hosting a device, ignoring");
            xSemaphoreGive(s_mu);
            return;
        }
        s_gone = false;
        s_nheld = 0;
        if (usb_host_device_open(s_client, event_msg->new_dev.address, &s_dev) != ESP_OK) {
            ESP_LOGE(TAG, "open failed");
            s_dev = NULL;
            xSemaphoreGive(s_mu);
            return;
        }

        const usb_device_desc_t *dd = NULL;
        if (usb_host_get_device_descriptor(s_dev, &dd) == ESP_OK && dd) {
            ESP_LOGI(TAG, "VID=%04X PID=%04X", dd->idVendor, dd->idProduct);
        }

        if (!claim_hid_interfaces()) {
            usb_host_device_close(s_client, s_dev);
            s_dev = NULL;
            xSemaphoreGive(s_mu);
            return;
        }
        if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + HID_DESC_MAX, 0, &s_ctrl) !=
            ESP_OK) {
            ESP_LOGE(TAG, "ctrl alloc failed");
            teardown_locked();
            xSemaphoreGive(s_mu);
            return;
        }
        xSemaphoreGive(s_mu);

        uint8_t cmd = WORK_SETUP;
        xQueueSend(s_work_q, &cmd, 0);
        return;
    }

    if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "device detached");
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_gone = true;
        halt_endpoints();
        if (s_inflight <= 0 && !s_busy_setup) {
            uint8_t cmd = WORK_TEARDOWN;
            xQueueSend(s_work_q, &cmd, 0);
        }
        xSemaphoreGive(s_mu);
    }
}

static void client_task(void *arg) {
    (void)arg;
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async =
            {
                .client_event_callback = client_event_cb,
                .callback_arg = NULL,
            },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void usb_daemon_task(void *arg) {
    (void)arg;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(client_task, "usb_client", 4096, NULL, 5, NULL);
    xTaskCreate(worker_task, "hid_worker", 8192, NULL, 5, NULL);

    uint32_t event_flags;
    while (true) {
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

void keybridge_usb_start(void) {
    s_mu = xSemaphoreCreateMutex();
    s_ctrl_done = xSemaphoreCreateBinary();
    s_work_q = xQueueCreate(4, sizeof(uint8_t));

    const esp_timer_create_args_t targs = {
        .callback = &consumer_timeout,
        .name = "hid_cons",
    };
    esp_timer_create(&targs, &s_consumer_timer);

    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 1);
    xTaskCreate(usb_daemon_task, "usb_daemon", 4096, NULL, 10, NULL);
}
