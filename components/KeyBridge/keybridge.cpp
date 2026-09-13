#include "keybridge.h"
#include "esphome/core/log.h"

static esphome::keybridge::KeyBridgeComponent *s_instance = nullptr;

extern "C" {
static void keybridge_hid_event_cb(uint16_t usage_page, uint16_t usage, uint8_t pressed) {
  if (s_instance != nullptr) {
    s_instance->on_hid_event(usage_page, usage, pressed);
  }
}
}

namespace esphome::keybridge {

static const char *const TAG = "KeyBridge";

void KeyBridgeComponent::set_key_sensor(uint16_t usage_page, uint16_t usage,
                                        binary_sensor::BinarySensor *sensor) {
  if (this->binding_count_ >= MAX_BINDINGS || sensor == nullptr) {
    return;
  }
  this->bindings_[this->binding_count_++] = KeyBinding{usage_page, usage, sensor};
}

void KeyBridgeComponent::on_hid_event(uint16_t usage_page, uint16_t usage, uint8_t pressed) {
  if (this->event_queue_ == nullptr) {
    return;
  }
  HidEvent ev{usage_page, usage, pressed};
  xQueueSend(this->event_queue_, &ev, 0);
}

void KeyBridgeComponent::setup() {
  for (size_t i = 0; i < this->binding_count_; i++) {
    this->bindings_[i].sensor->publish_initial_state(false);
  }

  this->event_queue_ = xQueueCreate(64, sizeof(HidEvent));
  s_instance = this;
  keybridge_set_hid_callback(keybridge_hid_event_cb);
  keybridge_usb_start();
}

void KeyBridgeComponent::loop() {
  if (this->event_queue_ == nullptr) {
    return;
  }

  HidEvent ev;
  while (xQueueReceive(this->event_queue_, &ev, 0) == pdTRUE) {
    if (ev.page == 0 && ev.usage == 0) {
      for (size_t i = 0; i < this->binding_count_; i++) {
        this->bindings_[i].sensor->publish_state(false);
      }
      continue;
    }

    for (size_t i = 0; i < this->binding_count_; i++) {
      if (this->bindings_[i].page == ev.page && this->bindings_[i].usage == ev.usage) {
        this->bindings_[i].sensor->publish_state(ev.pressed != 0);
      }
    }
  }
}

void KeyBridgeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "KeyBridge HID (%u key bindings)", (unsigned) this->binding_count_);
  for (size_t i = 0; i < this->binding_count_; i++) {
    ESP_LOGCONFIG(TAG, "  %04X:%04X", this->bindings_[i].page, this->bindings_[i].usage);
    LOG_BINARY_SENSOR("    ", "Key", this->bindings_[i].sensor);
  }
}

}  // namespace esphome::keybridge
