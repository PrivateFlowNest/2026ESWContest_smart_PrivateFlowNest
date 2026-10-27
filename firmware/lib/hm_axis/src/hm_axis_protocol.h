// ASCII line protocol of the doll axis controller (USB serial, 115200 8N1 on boards with a UART
// bridge; any host: PC, Jetson, Raspberry Pi). Standalone: not part of pfn.v1 / MQTT.
//
// Host -> controller: one command per line, terminated by "\n" (or "\r\n" / "\r"). Command words
// are case-insensitive, names are folded to lower case. Tokens are separated by spaces or tabs.
//   HOME                 start homing                       -> OK HOME, later EVT HOMED
//   MOVE <name|mm>       go to a named position or to mm    -> OK MOVE <label> <mm>, later
//                                                              EVT ARRIVED <label> <mm>
//   STOP                 brake and stop (always accepted)   -> OK STOP, later EVT STOPPED <mm>
//   ESTOP                stop at once, driver off, unhomed  -> OK ESTOP (+ EVT ERROR ESTOP if a
//                        (always accepted)                     motion was running)
//   POS?                 -> OK POS <mm> <state> <homed 0|1>
//   STATUS?              -> OK STATUS state=.. homed=0|1 pos=<mm> endstop=0|1 enabled=0|1 error=..
//   LIST                 -> OK LIST <count> <name>=<mm> ...
//   SET <name> <mm>      add/replace a named position (RAM only, lost at reset) -> OK SET <name> <mm>
//   ENABLE 0|1           driver off/on; ENABLE 0 also forgets the home position  -> OK ENABLE 0|1
//   VERSION?             -> OK VERSION <firmware> hm_axis/<version>
//   HELP                 -> OK HELP <command list>
// Every command line gets exactly one reply line: "OK ..." or "ERR <CODE> <detail>".
// Asynchronous lines start with "EVT ": BOOT <firmware> hm_axis/<version>, HOMED,
// ARRIVED <label> <mm>, STOPPED <mm>, ERROR <CODE> [detail]. Each HOME / MOVE ends with exactly
// one of HOMED / ARRIVED / STOPPED / ERROR.
// mm values are printed with two decimals. Accepted mm syntax: [+-]digits[.digits] (no exponent).
// Names: [a-z0-9_]{1,16} and not only digits (so a name can never be read as a number).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hm_axis.h"

namespace hm {
namespace axis {

const uint8_t kMaxNamedPositions = 8;
const uint8_t kNameMaxLength = 16;
const size_t kLineCapacity = 96;     // one command line: at most kLineCapacity - 1 characters
const size_t kOutputCapacity = 256;  // longest reply: LIST with 8 entries

// Compile-time default of a named position (board config header).
struct NamedPosition {
  const char* name;
  float mm;
};

bool isValidName(const char* name);
// Strict decimal: [+-]digits[.digits], at most 6 digits before and 6 after the point.
bool parseMm(const char* text, float* mm);
// "-12.34" style text of a value in hundredths of a millimetre.
void formatCentiMm(long centi_mm, char* out, size_t capacity);

class PositionTable {
 public:
  enum class Result : uint8_t { Ok, BadName, Full };
  struct Entry {
    char name[kNameMaxLength + 1];
    float mm;
  };

  PositionTable() : entries_(), count_(0) {}
  void clear() { count_ = 0; }
  // Adds a new name or replaces the value of an existing one. The name must already be valid
  // lower-case text (isValidName); the caller checks the range.
  Result set(const char* name, float mm);
  const Entry* find(const char* name) const;
  uint8_t count() const { return count_; }
  const Entry& at(uint8_t index) const { return entries_[index]; }

 private:
  Entry entries_[kMaxNamedPositions];
  uint8_t count_;
};

class LineSink {
 public:
  virtual ~LineSink() {}
  // One protocol line WITHOUT the line terminator (the board adds "\n").
  virtual void writeLine(const char* line) = 0;
};

class Controller {
 public:
  Controller(Axis& axis, LineSink& sink, const char* firmware_name);

  // Loads the default named positions and announces "EVT BOOT ...". Invalid defaults (bad name,
  // outside 0..travel, more than kMaxNamedPositions) make the axis CONFIG_INVALID. An invalid
  // config is also announced as "EVT ERROR CONFIG_INVALID <field>". Returns false in both cases.
  // Call after Axis::begin().
  bool begin(const NamedPosition* defaults, uint8_t count);

  // Feeds received bytes. Complete lines are executed at once.
  void feed(char byte);
  void feed(const char* data, size_t length);
  // Executes one command line (without terminator). Modifies the buffer (tokenizing).
  void handleLine(char* line);
  // Forwards pending axis events as EVT lines. Call after every Axis::tick().
  void poll();

  const PositionTable& positions() const { return table_; }

 private:
  static const size_t kMaxTokens = 4;

  void doMove(char* argument);
  void doSet(char* name, const char* value);
  void doList();
  void replyError(AxisError error);
  void reply(const char* format, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;
  void textForSteps(int32_t steps, char* out, size_t capacity) const;

  Axis& axis_;
  LineSink& sink_;
  const char* firmware_;
  PositionTable table_;
  char line_[kLineCapacity];
  size_t line_length_;
  bool line_overflow_;
  bool line_bad_byte_;
  char move_label_[kNameMaxLength + 16];
  char output_[kOutputCapacity];
};

}  // namespace axis
}  // namespace hm
