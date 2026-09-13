#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "keybridge_usb_driver.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <cstdint>

namespace esphome::keybridge {

struct KeyBinding {
  uint16_t page;
  uint16_t usage;
  binary_sensor::BinarySensor *sensor;
};

struct HidEvent {
  uint16_t page;
  uint16_t usage;
  uint8_t pressed;
};

class KeyBridgeComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_key_sensor(uint16_t usage_page, uint16_t usage, binary_sensor::BinarySensor *sensor);
  void on_hid_event(uint16_t usage_page, uint16_t usage, uint8_t pressed);

 protected:
  static const size_t MAX_BINDINGS = 192;
  KeyBinding bindings_[MAX_BINDINGS]{};
  size_t binding_count_{0};
  QueueHandle_t event_queue_{nullptr};
};

}  // namespace esphome::keybridge
