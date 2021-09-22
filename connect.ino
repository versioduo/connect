// © Kay Sievers <kay@vrfy.org>, 2019-2021
// SPDX-License-Identifier: Apache-2.0

#include <V2Color.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Timer.h>

V2DEVICE_METADATA("com.versioduo.connect", 20, "versioduo:samd:connect");

static V2Timer::Periodic Timer(3, 1000);
static V2LED::Basic LEDBuiltin(PIN_LED_ONBOARD, &Timer);
static V2LED::WS2812 LED(1, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
static V2Link::Port Socket(&SerialSocket);

static class Device : public V2Device {
public:
  Device() : V2Device() {
    metadata.vendor      = "Versio Duo";
    metadata.product     = "connect";
    metadata.description = "USB to V2 Socket Controller";
    metadata.home        = "https://versioduo.com/#connect";

    system.download       = "https://versioduo.com/download";
    system.configure      = "https://versioduo.com/configure";
    system.ports.announce = 2;

    configuration = {.magic{0x9dfe0000 | usb.pid}, .size{sizeof(config)}, .data{&config}};
  }

  // Config, written to EEPROM
  struct {
    struct {
      uint16_t pid;
    } usb;
  } config{.usb{.pid{usb.pid}}};

  void reset() {
    LED.setHSV(0, V2Color::Orange, 1, 0.5);
    allNotesOff();

    for (uint8_t i = 0; i < 15; i++) {
      _midi.setPort(i);
      Socket.send(_midi.set(0, V2MIDI::Packet::Status::SystemReset));
    }
  }

  void allNotesOff() {
    LEDBuiltin.reset();

    for (uint8_t i = 0; i < 15; i++) {
      _midi.setPort(i);
      Socket.send(_midi.setControlChange(0, V2MIDI::CC::AllNotesOff));
    }
  }

private:
  V2MIDI::Packet _midi{};
  V2Link::Packet _link;

  void handleInit() override {
    usb.pid = config.usb.pid;
  }

  void handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) override {
    if (channel != 0)
      return;

    switch (controller) {
      case V2MIDI::CC::AllSoundOff:
      case V2MIDI::CC::AllNotesOff:
        allNotesOff();
        break;
    }
  }

  void handleSystemReset() override {
    reset();
  }

  void importConfiguration(JsonObject json) override {
    JsonObject json_usb = json["usb"];
    if (json_usb) {
      const uint16_t pid = json_usb["pid"];
      if (pid > 0)
        config.usb.pid = pid;
    }
  }

  void exportConfiguration(JsonObject json) override {
    JsonObject json_usb = json.createNestedObject("usb");
    json_usb["#pid"]    = "The product id";
    json_usb["pid"]     = config.usb.pid;
  }
} Device;

// Dispatch MIDI packets
static class MIDI {
public:
  void loop() {
    if (!Device.usb.midi.receive(&_midi))
      return;

    LEDBuiltin.flash(0.03, 0.3);

    if (_midi.getPort() == 0) {
      Device.dispatch(&Device.usb.midi, &_midi);

    } else {
      _midi.setPort(_midi.getPort() - 1);
      Socket.send(&_midi);
    }
  }

private:
  V2MIDI::Packet _midi{};
} MIDI;

// Dispatch Link packets
static class Link : public V2Link {
public:
  Link() : V2Link(NULL, &Socket) {}

private:
  V2MIDI::Packet _midi{};

  // Forward children device events to the host
  void receiveSocket(V2Link::Packet *packet) override {
    if (packet->getType() == V2Link::Packet::Type::MIDI) {
      uint8_t address = packet->getAddress();
      if (address == 0x0f)
        return;

      LEDBuiltin.flash(0.005);

      if (Device.usb.midi.connected()) {
        packet->receive(&_midi);
        _midi.setPort(address + 1);
        Device.usb.midi.send(&_midi);
      }
    }
  }
} Link;

void TC3_Handler() {
  LEDBuiltin.tick();
  Timer.clear();
}

void setup() {
  Serial.begin(9600);

  LED.begin();
  LED.setMaxBrightness(0.5);
  Socket.begin();

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. This needs to be after begin().
  setSerialPriority(&SerialSocket, 2);

  Timer.begin();
  Device.begin();
  Device.reset();
}

void loop() {
  LEDBuiltin.loop();
  LED.loop();
  MIDI.loop();
  Link.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
