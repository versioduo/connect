#include <V2Buttons.h>
#include <V2Device.h>
#include <V2Link.h>
#include <V2MIDI.h>

V2DEVICE_METADATA("com.versioduo.connect-hub", 2, "versioduo:samd:connect-hub");

namespace {
  V2LED::WS2812        LED{20, PIN_LED_WS2812, &sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM};
  V2Link::Port         Socket{&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE};
  V2Link::Port         Socket2{&SerialSocket2, PIN_SERIAL_SOCKET2_TX_ENABLE};
  V2MIDI::SerialDevice MIDISerial{&SerialMIDI};
  V2MIDI::SerialDevice MIDISerial2{&SerialMIDI2};

  struct USBPort {
    enum Type : uint8_t {
      Hub,
      Socket,
      Socket2,
      Serial,
      Serial2,
    };
  };

  class Ping {
  public:
    Ping() = delete;
    Ping(V2Link::Port& s, V2Colour::Hue c) : _socket{s}, _color(c) {}

    auto loop() {
      if (V2Base::getUsecSince(_usec) < 1000 * 1000)
        return;

      _usec = V2Base::getUsec();

      switch (_state) {
        case State::Running:
          if (_expect > 0) {
            _state      = State::Failed;
            _failedMsec = millis();
            break;
          }
          [[fallthrough]];

        case State::Init:
          _sequence += 2;
          _link.number(_sequence);
          _socket.send(_link);
          _expect = _sequence + 1;
          break;

        case State::Failed:
          LED.splashHSV(0.01, _color, 0.9, 0.5);
          break;
      }
    }

    auto receive(uint32_t number) {
      switch (_state) {
        case State::Init:
          _state     = State::Running;
          _startMsec = millis();
          _start     = _sequence;
          LED.setHSV(0, V2Colour::Cyan, 0.9, 0.6);
          [[fallthrough]];

        case State::Running:
          if (number == _expect) {
            _expect = 0;
            break;
          }

          _state      = State::Failed;
          _failedMsec = millis();
          break;
      }
    }

    auto started() -> std::optional<uint32_t> const {
      if (_startMsec == 0)
        return {};

      return _startMsec / 1000;
    }

    auto failed() -> std::optional<uint32_t> const {
      if (_failedMsec == 0)
        return {};

      return _failedMsec / 1000;
    }

    auto reset() {
      _state      = {};
      _usec       = 0;
      _sequence   = 0;
      _start      = 0;
      _expect     = 0;
      _startMsec  = 0;
      _failedMsec = 0;
    }

  private:
    V2Link::Port& _socket;
    V2Colour::Hue _color{};
    enum class State { Init, Running, Failed } _state{};
    uint32_t       _usec{};
    uint32_t       _sequence{};
    uint32_t       _start{};
    uint32_t       _expect{};
    uint32_t       _startMsec{};
    uint32_t       _failedMsec{};
    V2Link::Packet _link;
  };

  Ping PingSocket(Socket, V2Colour::Red);
  Ping PingSocket2(Socket2, V2Colour::Blue);

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 connect-hub";
      metadata.description = "MIDI Hub";
      metadata.home        = "https://versioduo.com/#connect-hub";

      system.download  = "https://versioduo.com/download";
      system.configure = "https://versioduo.com/configure";

      help.device = "MIDI message router with two V2 Link Sockets and two MIDI Serial ports. The USB Power Delivery "
                    "port supplies the V2 Link Sockets with 9 V power.";

      help.configuration = "MIDI messages can be routed between all four MIDI ports. Every port specifies "
                           "from which port it wants to receive messages. Notes and Control Messages are "
                           "enabled seperately.\n"
                           "The USB MIDI connection exposes all four MIDI ports separately. USB always "
                           "receives all messages from all ports, and can send to any port without any filter "
                           "getting applied.";

      usb.ports.standard = 5;
      configuration      = {.size{sizeof(config)}, .data{&config}};
    }

    auto route(USBPort::Type from, V2MIDI::Packet& m) {
      m.port = from;
      usb.midi.send(m);
      m.port = 0;

      auto filter{[&m](const Route::Filter& f, V2MIDI::Transport& port) {
        if (f.notes) {
          switch (m.type()) {
            case V2MIDI::Packet::Status::NoteOff:
            case V2MIDI::Packet::Status::NoteOn:
            case V2MIDI::Packet::Status::Aftertouch:
            case V2MIDI::Packet::Status::ProgramChange:
            case V2MIDI::Packet::Status::AftertouchChannel:
            case V2MIDI::Packet::Status::PitchBend:
              port.send(m);
              return;
          }
        }

        if (f.controller)
          port.send(m);
      }};

      switch (from) {
        case USBPort::Socket:
          filter(config.socket.socket2, Socket2);
          filter(config.socket.serial, MIDISerial);
          filter(config.socket.serial2, MIDISerial2);
          break;

        case USBPort::Socket2:
          filter(config.socket2.socket, Socket2);
          filter(config.socket2.serial, MIDISerial);
          filter(config.socket2.serial2, MIDISerial2);
          break;

        case USBPort::Serial:
          filter(config.serial.socket, Socket);
          filter(config.serial.socket2, Socket2);
          filter(config.serial.serial2, MIDISerial2);
          break;

        case USBPort::Serial2:
          filter(config.serial2.socket, Socket);
          filter(config.serial2.socket2, Socket2);
          filter(config.serial2.serial, MIDISerial);
          break;
      }
    }

  private:
    struct Route {
      struct Filter {
        bool notes{};
        bool controller{};
      };

      struct {
        Filter socket2;
        Filter serial;
        Filter serial2;
      } socket;

      struct {
        Filter socket;
        Filter serial;
        Filter serial2;
      } socket2;

      struct {
        Filter socket;
        Filter socket2;
        Filter serial2;
      } serial;

      struct {
        Filter socket;
        Filter socket2;
        Filter serial;
      } serial2;
    } config;

    enum class CC {
      Rainbow = V2MIDI::CC::Controller90,
    };

    auto handleReset() -> void override {
      LED.reset();
      LED.setHSV(0, V2Colour::Orange, 0.9, 0.6);
      PingSocket.reset();
      PingSocket2.reset();
    }

    auto handleSend(V2MIDI::Packet* midi) -> bool override {
      led.flash(0.03, 0.3);
      usb.midi.send(*midi);
      return true;
    }

    auto handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) -> void override {
      LED.splashHSV(0.5, V2Colour::Orange, 1, 0.25);
    }

    auto handleSystemReset() -> void override {
      reset();
    }

    auto exportSystem(JsonObject json) -> void override {
      auto ping{json["ping"].to<JsonObject>()};
      if (PingSocket.started()) {
        auto j{ping["Socket"].to<JsonObject>()};
        j["started"] = PingSocket.started().value();
        if (PingSocket.failed())
          j["failed"] = PingSocket.failed().value();
      }

      if (PingSocket2.started()) {
        auto j{ping["Socket-2"].to<JsonObject>()};
        j["started"] = PingSocket2.started().value();
        if (PingSocket2.failed())
          j["failed"] = PingSocket2.failed().value();
      }
    }

    void importConfiguration(JsonObject json) override {
      auto route{json["route"]};
      if (!route)
        return;

      if (auto port{route["Socket"]}; port) {
        if (auto j{port["Socket-2"]}; j) {
          if (!j["notes"].isNull())
            config.socket.socket2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket.socket2.controller = j["controller"];
        }

        if (auto j{port["Serial"]}; j) {
          if (!j["notes"].isNull())
            config.socket.serial.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket.serial.controller = j["controller"];
        }

        if (auto j{port["Serial-2"]}; j) {
          if (!j["notes"].isNull())
            config.socket.serial2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket.serial2.controller = j["controller"];
        }
      }

      if (auto port{route["Socket-2"]}; port) {
        if (auto j{port["Socket"]}; j) {
          if (!j["notes"].isNull())
            config.socket2.socket.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket2.socket.controller = j["controller"];
        }

        if (auto j{port["Serial"]}; j) {
          if (!j["notes"].isNull())
            config.socket2.serial.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket2.serial.controller = j["controller"];
        }

        if (auto j{port["Serial-2"]}; j) {
          if (!j["notes"].isNull())
            config.socket2.serial2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.socket2.serial2.controller = j["controller"];
        }
      }

      if (auto port{route["Serial"]}; port) {
        if (auto j{port["Socket"]}; j) {
          if (!j["notes"].isNull())
            config.serial.socket.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial.socket.controller = j["controller"];
        }

        if (auto j{port["Socket-2"]}; j) {
          if (!j["notes"].isNull())
            config.serial.socket2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial.socket2.controller = j["controller"];
        }

        if (auto j{port["Serial-2"]}; j) {
          if (!j["notes"].isNull())
            config.serial.serial2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial.serial2.controller = j["controller"];
        }
      }

      if (auto port{route["Serial-2"]}; port) {
        if (auto j{port["Socket"]}; j) {
          if (!j["notes"].isNull())
            config.serial2.socket.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial2.socket.controller = j["controller"];
        }

        if (auto j{port["Socket-2"]}; j) {
          if (!j["notes"].isNull())
            config.serial2.socket2.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial2.socket2.controller = j["controller"];
        }

        if (auto j{port["Serial"]}; j) {
          if (!j["notes"].isNull())
            config.serial2.serial.notes = j["notes"];

          if (!j["controller"].isNull())
            config.serial2.serial.controller = j["controller"];
        }
      }
    }

    void exportConfiguration(JsonObject json) override {
      json["#route"] = "Route Notes and Control Messages";
      auto route{json["route"].to<JsonObject>()};

      {
        auto port{route["Socket"].to<JsonObject>()};
        {
          auto j{port["Socket-2"].to<JsonObject>()};
          j["notes"]      = config.socket.socket2.notes;
          j["controller"] = config.socket.socket2.controller;
        }
        {
          auto j{port["Serial"].to<JsonObject>()};
          j["notes"]      = config.socket.serial.notes;
          j["controller"] = config.socket.serial.controller;
        }
        {
          auto j{port["Serial-2"].to<JsonObject>()};
          j["notes"]      = config.socket.serial2.notes;
          j["controller"] = config.socket.serial2.controller;
        }
      }

      {
        auto port{route["Socket-2"].to<JsonObject>()};
        {
          auto j{port["Socket"].to<JsonObject>()};
          j["notes"]      = config.socket2.socket.notes;
          j["controller"] = config.socket2.socket.controller;
        }
        {
          auto j{port["Serial"].to<JsonObject>()};
          j["notes"]      = config.socket2.serial.notes;
          j["controller"] = config.socket2.serial.controller;
        }
        {
          auto j{port["Serial-2"].to<JsonObject>()};
          j["notes"]      = config.socket2.serial2.notes;
          j["controller"] = config.socket2.serial2.controller;
        }
      }

      {
        auto port{route["Serial"].to<JsonObject>()};
        {
          auto j{port["Socket"].to<JsonObject>()};
          j["notes"]      = config.serial.socket.notes;
          j["controller"] = config.serial.socket.controller;
        }
        {
          auto j{port["Socket-2"].to<JsonObject>()};
          j["notes"]      = config.serial.socket2.notes;
          j["controller"] = config.serial.socket2.controller;
        }
        {
          auto j{port["Serial-2"].to<JsonObject>()};
          j["notes"]      = config.serial.serial2.notes;
          j["controller"] = config.serial.serial2.controller;
        }
      }

      {
        auto port{route["Serial-2"].to<JsonObject>()};
        {
          auto j{port["Socket"].to<JsonObject>()};
          j["notes"]      = config.serial2.socket.notes;
          j["controller"] = config.serial2.socket.controller;
        }
        {
          auto j{port["Socket-2"].to<JsonObject>()};
          j["notes"]      = config.serial2.socket2.notes;
          j["controller"] = config.serial2.socket2.controller;
        }
        {
          auto j{port["Serial"].to<JsonObject>()};
          j["notes"]      = config.serial2.serial.notes;
          j["controller"] = config.serial2.serial.controller;
        }
      }
    }

    auto exportSettings(JsonArray json) -> void override {
      {
        auto j{json.add<JsonObject>()};
        j["type"]  = "title";
        j["title"] = "Socket";
        j["subtitle"] = "Forward Incoming MIDI";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket-2";
        j["path"] = "route/Socket/Socket-2";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial";
        j["path"] = "route/Socket/Serial";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial-2";
        j["path"] = "route/Socket/Serial-2";
      }

      {
        auto j{json.add<JsonObject>()};
        j["type"]  = "title";
        j["title"] = "Socket-2";
        j["subtitle"] = "Forward Incoming MIDI";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket";
        j["path"] = "route/Socket-2/Socket";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial";
        j["path"] = "route/Socket-2/Serial";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial-2";
        j["path"] = "route/Socket-2/Serial-2";
      }

      {
        auto j{json.add<JsonObject>()};
        j["type"]  = "title";
        j["title"] = "Serial";
        j["subtitle"] = "Forward Incoming MIDI";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket";
        j["path"] = "route/Serial/Socket";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket-2";
        j["path"] = "route/Serial/Socket-2";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial-2";
        j["path"] = "route/Serial/Serial-2";
      }

      {
        auto j{json.add<JsonObject>()};
        j["type"]  = "title";
        j["title"] = "Serial-2";
        j["subtitle"] = "Forward Incoming MIDI";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket";
        j["path"] = "route/Serial-2/Socket";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Socket-2";
        j["path"] = "route/Serial-2/Socket-2";
      }
      {
        auto j{json.add<JsonObject>()};
        j["type"] = "filter";
        j["text"] = "Serial-2";
        j["path"] = "route/Serial-2/Serial";
      }
    }
  } Device;

  // Dispatch MIDI packets
  class MIDI {
  public:
    auto loop() {
      if (Device.usb.midi.receive(_midi)) {
        switch (_midi.port) {
          case USBPort::Hub:
            Device.dispatch(&Device.usb.midi, &_midi);
            break;

          case USBPort::Socket: {
            V2Link::Packet p(0, _midi);
            p.midi.port = 0;
            Socket.send(p);
            break;
          }

          case USBPort::Socket2: {
            V2Link::Packet p(0, _midi);
            p.midi.port = 0;
            Socket2.send(p);
            break;
          }
        }
      }

      if (MIDISerial.receive(_midi))
        Device.route(USBPort::Serial, _midi);

      if (MIDISerial2.receive(_midi))
        Device.route(USBPort::Serial2, _midi);
    }

  private:
    V2MIDI::Packet _midi;
  } MIDI;

  // Dispatch Link packets.
  class Link : public V2Link {
  public:
    Link() : V2Link(nullptr, &Socket, &Socket2) {
      Device.link = this;
    }

  private:
    // Forward children device events to the host.
    auto receiveSocket(V2Link::Packet& p) -> void override {
      if (p.address != 1)
        return;

      switch (p.type) {
        case V2Link::Packet::Type::MIDI: {
          static constexpr std::array<uint8_t, 16> channel{7, 11, 15, 19, 6, 10, 14, 18, 5, 9, 13, 17, 4, 8, 12, 16};
          switch (p.midi.type()) {
            case V2MIDI::Packet::Status::NoteOn:
              LED.setHSV(channel[p.midi.channel()], V2Colour::Orange, 0.9, 0.8);
              break;

            case V2MIDI::Packet::Status::NoteOff:
              LED.setBrightness(channel[p.midi.channel()], 0);
              break;

            case V2MIDI::Packet::Status::ControlChange:
              LED.splashHSV(0.005, channel[p.midi.channel()], 1, V2Colour::Cyan, 0.9, 0.2);
              break;
          }

          Device.route(USBPort::Socket, p.midi);
          break;
        }

        case V2Link::Packet::Type::Number:
          PingSocket.receive(p.number());
          break;
      }
    }

    auto receiveSocketNode(V2Link::Packet& p) -> void override {
      if (p.address != 0)
        return;

      switch (p.type) {
        case V2Link::Packet::Type::MIDI:
          Device.route(USBPort::Socket2, p.midi);
          break;

        case V2Link::Packet::Type::Number:
          PingSocket2.receive(p.number());
          break;
      }
    }
  } Link;

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

          auto sendReset{[this](V2MIDI::Transport& port) {
            for (uint8_t i{}; i < 16; i++) {
              _midi.setControlChange(i, V2MIDI::CC::AllSoundOff, 0);
              port.send(_midi);
              _midi.setControlChange(i, V2MIDI::CC::AllNotesOff, 0);
              port.send(_midi);
            }
          }};

          sendReset(Device.usb.midi);
          sendReset(Socket);
          sendReset(Socket2);
          sendReset(MIDISerial);
          sendReset(MIDISerial2);
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

  Device.usb.midi.setPortName(USBPort::Socket + 1, "Socket");
  Device.usb.midi.setPortName(USBPort::Socket2 + 1, "Socket-2");
  Device.usb.midi.setPortName(USBPort::Serial + 1, "Serial");
  Device.usb.midi.setPortName(USBPort::Serial2 + 1, "Serial-2");

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. The call needs to be after begin().
  Link.begin();
  setSerialPriority(&SerialSocket, 2);
  setSerialPriority(&SerialSocket2, 2);

  MIDISerial.begin();
  MIDISerial2.begin();
  Device.serial = &MIDISerial;
  for (auto& b : Buttons)
    b.begin();

  Device.begin();
  Device.reset();
}

auto loop() -> void {
  PingSocket.loop();
  PingSocket2.loop();
  LED.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Device.loop();

  if (Device.idle())
    Device.sleep();
}
