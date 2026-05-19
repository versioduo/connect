#include <V2Buttons.h>
#include <V2Device.h>
#include <V2Link.h>
#include <V2MIDI.h>

V2DEVICE_METADATA("com.versioduo.connect-socket", 1, "versioduo:samd:connect-socket");

namespace {
  V2LED::WS2812        LED{20, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM};
  V2Link::Port         Socket{&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE};
  V2MIDI::SerialDevice MIDISerial{&SerialMIDI};

  class {
  public:
    auto loop() {
      if (V2Base::getUsecSince(_usec) < 1000 * 1000)
        return;

      _usec = V2Base::getUsec();

      switch (_state) {
        case State::Running:
          if (_expect > 0) {
            _state = State::Failed;
            break;
          }
          [[fallthrough]];

        case State::Init:
          _sequence += 2;
          _packet.setNumber(_sequence);
          Socket.send(0, &_packet);
          _expect = _sequence + 1;
          break;

        case State::Failed:
          LED.splashHSV(0.01, V2Colour::Red, 0.9, 0.5);
          break;
      }
    }

    auto receive(uint32_t number) {
      switch (_state) {
        case State::Init:
          _state = State::Running;
          LED.setHSV(0, V2Colour::Cyan, 0.9, 0.6);
          [[fallthrough]];

        case State::Running:
          if (number == _expect) {
            _expect = 0;
            break;
          }

          _state = State::Failed;
          break;
      }
    }

    auto sequence() -> uint32_t const {
      return _sequence;
    }

    auto reset() {
      _state    = {};
      _usec     = 0;
      _sequence = 0;
      _expect   = 0;
    }

  private:
    enum class State { Init, Running, Failed } _state{};
    uint32_t       _usec{};
    uint32_t       _sequence{};
    uint32_t       _expect{};
    V2Link::Packet _packet;
  } Ping;

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 connect-socket";
      metadata.description = "MIDI Connector";
      metadata.home        = "https://versioduo.com/#connect";

      system.download  = "https://versioduo.com/download";
      system.configure = "https://versioduo.com/configure";

      usb.ports.standard = 2;
    }

    enum class CC {
      BeatLength = V2MIDI::CC::Controller14,
      Rainbow    = V2MIDI::CC::Controller90,
    };

    auto handleReset() -> void override {
      LED.reset();
      Ping.reset();
    }

    auto handleSend(V2MIDI::Packet* midi) -> bool override {
      led.flash(0.03, 0.3);
      usb.midi.send(midi);
      return true;
    }

    auto handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) -> void override {
      LED.splashHSV(0.5, V2Colour::Orange, 1, 0.25);
    }

    auto handleSystemReset() -> void override {
      reset();
    }

    auto exportSystem(JsonObject json) -> void override {
      JsonObject j{json["ping"].to<JsonObject>()};
      j["sequence"] = Ping.sequence();
    }
  } Device;

  // Dispatch Link packets.
  class Link : public V2Link {
  public:
    Link() : V2Link(nullptr, &Socket) {
      Device.link = this;
    }

  private:
    V2MIDI::Packet _midi{};

    // Forward children device events to the host.
    auto receiveSocket(V2Link::Packet* packet) -> void override {
      switch (packet->getType()) {
        case V2Link::Packet::Type::MIDI: {
          auto address{packet->getAddress()};
          if (address == 0x0f)
            return;

          if (address > 0)
            return;

          packet->receive(&_midi);
          MIDISerial.send(&_midi);

          static constexpr std::array<uint8_t, 16> channel{7, 11, 15, 19, 6, 10, 14, 18, 5, 9, 13, 17, 4, 8, 12, 16};
          switch (_midi.getType()) {
            case V2MIDI::Packet::Status::NoteOn:
              LED.setHSV(channel[_midi.getChannel()], V2Colour::Orange, 0.9, 0.8);
              break;

            case V2MIDI::Packet::Status::NoteOff:
              LED.setBrightness(channel[_midi.getChannel()], 0);
              break;

            case V2MIDI::Packet::Status::ControlChange:
              LED.setHSV(channel[_midi.getChannel()], V2Colour::Cyan, 0.9, 0.8);
              break;
          }

          if (!Device.usb.midi.connected())
            return;

          _midi.setPort(address + 1);
          Device.usb.midi.send(&_midi);
        } break;

        case V2Link::Packet::Type::Number:
          Ping.receive(packet->getNumber());
          break;
      }
    }
  } Link;

  // Dispatch MIDI packets
  class MIDI {
  public:
    auto loop() {
      if (Device.usb.midi.receive(&_midi)) {
        if (_midi.getPort() == 0) {
          Device.dispatch(&Device.usb.midi, &_midi);

        } else {
          _midi.setPort(_midi.getPort() - 1);
          Socket.send(&_midi);
        }
      }

      if (MIDISerial.receive(&_midi))
        Socket.send(&_midi);
    }

  private:
    V2MIDI::Packet _midi;
  } MIDI;

  class Button : public V2Buttons::Button {
  public:
    enum class Function {
      Main,
      Second,
      Three,
      Four,
    };
    Button(Function function, uint8_t pin) : V2Buttons::Button(&_config, pin), _function{function} {}

  private:
    const Function          _function;
    const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};
    V2MIDI::Packet          _midi;

    auto handleHold(uint8_t count) -> void override {
      switch (_function) {
        case Function::Main:
          switch (count) {
            case 0:
              Device.send(V2MIDI::Packet().setControlChange(0, 3, 0));
              LED.rainbow(1, 2, 0.8);
              break;
          }
          break;
      }
    }

    auto handleClick(uint8_t count) -> void override {
      switch (_function) {
        case Function::Main:
          Device.reset();
          for (uint8_t i{}; i < 16; i++) {
            _midi.setControlChange(i, V2MIDI::CC::AllSoundOff, 0);
            Device.usb.midi.send(&_midi);
            MIDISerial.send(&_midi);
            _midi.setControlChange(i, V2MIDI::CC::AllNotesOff, 0);
            Device.usb.midi.send(&_midi);
            MIDISerial.send(&_midi);
          }
          break;
      }
    }
  } Buttons[]{
    Button{Button::Function::Main, PIN_BUTTON + 0},
    Button{Button::Function::Second, PIN_BUTTON + 1},
    Button{Button::Function::Three, PIN_BUTTON + 2},
    Button{Button::Function::Four, PIN_BUTTON + 3},
  };
}

auto setup() -> void {
  Serial.begin(9600);
  LED.begin();
  LED.setMaxBrightness(0.2);
  Link.begin();
  setSerialPriority(&SerialSocket, 2);
  MIDISerial.begin();
  Device.serial = &MIDISerial;
  for (auto& b : Buttons)
    b.begin();
  Device.usb.midi.setPortName(1, "Local");
  Device.usb.midi.setPortName(2, "Remote");
  Device.begin();
  Device.reset();
}

auto loop() -> void {
  Ping.loop();
  LED.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
