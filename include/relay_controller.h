#pragma once

class RelayController {
public:
  void begin();
  void set(bool on);
  void toggle();
  bool isOn() const;
private:
  bool relayOn_ = true;
};

