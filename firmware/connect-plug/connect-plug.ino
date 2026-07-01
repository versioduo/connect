#include <V2Buttons.h>
#include <V2Device.h>
#include <V2Link.h>
#include <V2MIDI.h>

V2DEVICE_METADATA("com.versioduo.connect-plug", 3, "versioduo:samd:connect-plug");

namespace {
  V2Link::Port         Plug{&SerialPlug, PIN_SERIAL_PLUG_TX_ENABLE};
  V2MIDI::SerialDevice MIDISerial{&SerialMIDI};

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 connect-plug";
      metadata.description = "MIDI Connector";
      metadata.home        = "https://versioduo.com/#connect-plug";

      system.download  = "https://versioduo.com/download";
      system.configure = "https://versioduo.com/configure";

      usb.ports.standard = 2;
    }

    auto handleSend(V2MIDI::Packet* midi) -> bool override {
      led.flash(0.03, 0.3);
      usb.midi.send(*midi);
      Plug.send(*midi);
      return true;
    }

    auto handleSystemReset() -> void override {
      reset();
    }
  } Device;

  // Dispatch MIDI packets
  class MIDI {
  public:
    auto loop() {
      if (Device.usb.midi.receive(_midi)) {
        switch (_midi.port) {
          case 0:
            Device.dispatch(&Device.usb.midi, &_midi);
            break;

          case 1:
            _midi.port = 0;
            Plug.send(_midi);
            break;
        }
      }

      if (MIDISerial.receive(_midi))
        Device.send(&_midi);
    }

  private:
    V2MIDI::Packet _midi;
  } MIDI;

  // Dispatch Link packets.
  class Link : public V2Link {
  public:
    Link() : V2Link(&Plug, nullptr) {
      Device.link = this;
    }

  private:
    auto receivePlug(V2Link::Packet& p) -> void override {
      switch (p.type) {
        case V2Link::Packet::Type::MIDI:
          Device.dispatch(&Plug, &p.midi);
          Device.usb.midi.send(p.midi);
          MIDISerial.send(p.midi);
          break;

        case V2Link::Packet::Type::Number: {
          // The sender pings with even numbers, we reply with an odd number.
          p.number(p.number() + 1);
          Plug.send(p);
          break;
        }
      }
    }
  } Link;
}

auto setup() -> void {
  Serial.begin(9600);

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. The call needs to be after begin().
  Link.begin();
  setSerialPriority(&SerialPlug, 2);

  MIDISerial.begin();
  Device.serial = &MIDISerial;
  Device.usb.midi.setPortName(1, "Local");
  Device.usb.midi.setPortName(2, "Remote");
  Device.begin();
  Device.reset();
}

auto loop() -> void {
  MIDI.loop();
  Link.loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
