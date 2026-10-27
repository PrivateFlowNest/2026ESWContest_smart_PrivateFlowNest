#include "hm_axis_protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace hm {
namespace axis {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

void toLower(char* text) {
  for (; *text != '\0'; ++text) {
    if (*text >= 'A' && *text <= 'Z') *text = static_cast<char>(*text - 'A' + 'a');
  }
}

// Case-insensitive match of a command word against an upper-case keyword.
bool keyword(const char* token, const char* word) {
  while (*token != '\0' && *word != '\0') {
    if (upper(*token) != *word) return false;
    ++token;
    ++word;
  }
  return *token == '\0' && *word == '\0';
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

bool isValidName(const char* name) {
  if (name == nullptr) return false;
  size_t length = 0;
  bool has_non_digit = false;
  for (; name[length] != '\0'; ++length) {
    if (length >= kNameMaxLength) return false;
    const char c = name[length];
    if ((c >= 'a' && c <= 'z') || c == '_') {
      has_non_digit = true;
    } else if (!isDigit(c)) {
      return false;
    }
  }
  return length > 0 && has_non_digit;
}

bool parseMm(const char* text, float* mm) {
  if (text == nullptr) return false;
  const char* p = text;
  bool negative = false;
  if (*p == '+' || *p == '-') {
    negative = *p == '-';
    ++p;
  }
  long whole = 0;
  int whole_digits = 0;
  while (isDigit(*p)) {
    if (++whole_digits > 6) return false;
    whole = whole * 10 + (*p - '0');
    ++p;
  }
  long fraction = 0;
  long scale = 1;
  int fraction_digits = 0;
  if (*p == '.') {
    ++p;
    while (isDigit(*p)) {
      if (++fraction_digits > 6) return false;
      fraction = fraction * 10 + (*p - '0');
      scale *= 10;
      ++p;
    }
  }
  if (*p != '\0' || whole_digits + fraction_digits == 0) return false;
  const double value = static_cast<double>(whole) + static_cast<double>(fraction) / scale;
  if (mm != nullptr) *mm = static_cast<float>(negative ? -value : value);
  return true;
}

void formatCentiMm(long centi_mm, char* out, size_t capacity) {
  const unsigned long magnitude =
      centi_mm < 0 ? static_cast<unsigned long>(-centi_mm) : static_cast<unsigned long>(centi_mm);
  snprintf(out, capacity, "%s%lu.%02lu", centi_mm < 0 ? "-" : "", magnitude / 100ul,
           magnitude % 100ul);
}

PositionTable::Result PositionTable::set(const char* name, float mm) {
  if (!isValidName(name)) return Result::BadName;
  for (uint8_t index = 0; index < count_; ++index) {
    if (strcmp(entries_[index].name, name) == 0) {
      entries_[index].mm = mm;
      return Result::Ok;
    }
  }
  if (count_ >= kMaxNamedPositions) return Result::Full;
  Entry& entry = entries_[count_++];
  strncpy(entry.name, name, kNameMaxLength);
  entry.name[kNameMaxLength] = '\0';
  entry.mm = mm;
  return Result::Ok;
}

const PositionTable::Entry* PositionTable::find(const char* name) const {
  if (name == nullptr) return nullptr;
  for (uint8_t index = 0; index < count_; ++index) {
    if (strcmp(entries_[index].name, name) == 0) return &entries_[index];
  }
  return nullptr;
}

Controller::Controller(Axis& axis, LineSink& sink, const char* firmware_name)
    : axis_(axis), sink_(sink), firmware_(firmware_name != nullptr ? firmware_name : "hm_axis"),
      table_(), line_(), line_length_(0), line_overflow_(false), line_bad_byte_(false),
      move_label_(), output_() {}

bool Controller::begin(const NamedPosition* defaults, uint8_t count) {
  table_.clear();
  line_length_ = 0;
  line_overflow_ = false;
  line_bad_byte_ = false;
  move_label_[0] = '\0';
  if (axis_.configProblem() == nullptr) {
    const char* problem = nullptr;
    if (count > kMaxNamedPositions || (count > 0 && defaults == nullptr)) problem = "positions";
    for (uint8_t index = 0; problem == nullptr && index < count; ++index) {
      if (!isValidName(defaults[index].name) || !axis_.targetForMm(defaults[index].mm, nullptr) ||
          table_.set(defaults[index].name, defaults[index].mm) != PositionTable::Result::Ok) {
        problem = "positions";
      }
    }
    if (problem != nullptr) axis_.invalidateConfig(problem);
  }
  reply("EVT BOOT %s hm_axis/%s", firmware_, kAxisLibVersion);
  if (axis_.configProblem() != nullptr) {
    reply("EVT ERROR CONFIG_INVALID %s", axis_.configProblem());
    return false;
  }
  return true;
}

void Controller::feed(const char* data, size_t length) {
  for (size_t index = 0; index < length; ++index) feed(data[index]);
}

void Controller::feed(char byte) {
  if (byte == '\n' || byte == '\r') {
    if (line_overflow_) {
      reply("ERR LINE_TOO_LONG max %u chars", static_cast<unsigned>(kLineCapacity - 1));
    } else if (line_bad_byte_) {
      reply("ERR BAD_LINE non-ASCII or control byte");
    } else if (line_length_ > 0) {
      line_[line_length_] = '\0';
      handleLine(line_);
    }
    line_length_ = 0;
    line_overflow_ = false;
    line_bad_byte_ = false;
    return;
  }
  if (line_overflow_) return;  // discard the rest of an over-long line
  const unsigned char value = static_cast<unsigned char>(byte);
  if ((value < 0x20 && byte != '\t') || value >= 0x7f) {
    line_bad_byte_ = true;
    return;
  }
  if (line_length_ >= kLineCapacity - 1) {
    line_overflow_ = true;
    return;
  }
  line_[line_length_++] = byte;
}

void Controller::handleLine(char* line) {
  char* tokens[kMaxTokens];
  size_t count = 0;
  bool too_many = false;
  char* p = line;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t') *p++ = '\0';
    if (*p == '\0') break;
    if (count == kMaxTokens) {
      too_many = true;
      break;
    }
    tokens[count++] = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') ++p;
  }
  if (count == 0) return;  // blank line: no reply
  const char* command = tokens[0];
  const size_t args = too_many ? kMaxTokens : count - 1;

  // Safety commands are honoured whatever follows them on the line.
  if (keyword(command, "ESTOP")) {
    axis_.estop();
    reply("OK ESTOP");
    return;
  }
  if (keyword(command, "STOP")) {
    axis_.stop();
    reply("OK STOP");
    return;
  }

  if (keyword(command, "HOME")) {
    if (args != 0) {
      reply("ERR BAD_ARG usage: HOME");
      return;
    }
    const AxisError error = axis_.home();
    if (error != AxisError::None) {
      replyError(error);
    } else {
      reply("OK HOME");
    }
  } else if (keyword(command, "MOVE")) {
    if (args != 1) {
      reply("ERR BAD_ARG usage: MOVE <name|mm>");
      return;
    }
    doMove(tokens[1]);
  } else if (keyword(command, "POS?") || keyword(command, "POS")) {
    if (args != 0) {
      reply("ERR BAD_ARG usage: POS?");
      return;
    }
    char mm[16];
    textForSteps(axis_.position(), mm, sizeof(mm));
    reply("OK POS %s %s %d", mm, stateName(axis_.state()), axis_.homed() ? 1 : 0);
  } else if (keyword(command, "STATUS?") || keyword(command, "STATUS")) {
    if (args != 0) {
      reply("ERR BAD_ARG usage: STATUS?");
      return;
    }
    char mm[16];
    textForSteps(axis_.position(), mm, sizeof(mm));
    reply("OK STATUS state=%s homed=%d pos=%s endstop=%d enabled=%d error=%s",
          stateName(axis_.state()), axis_.homed() ? 1 : 0, mm, axis_.endstopActive() ? 1 : 0,
          axis_.enabled() ? 1 : 0, errorName(axis_.lastError()));
  } else if (keyword(command, "LIST") || keyword(command, "LIST?")) {
    if (args != 0) {
      reply("ERR BAD_ARG usage: LIST");
      return;
    }
    doList();
  } else if (keyword(command, "SET")) {
    if (args != 2) {
      reply("ERR BAD_ARG usage: SET <name> <mm>");
      return;
    }
    doSet(tokens[1], tokens[2]);
  } else if (keyword(command, "ENABLE")) {
    if (args != 1 || (strcmp(tokens[1], "0") != 0 && strcmp(tokens[1], "1") != 0)) {
      reply("ERR BAD_ARG usage: ENABLE 0|1");
      return;
    }
    const bool on = tokens[1][0] == '1';
    const AxisError error = axis_.setEnabled(on);
    if (error != AxisError::None) {
      replyError(error);
    } else {
      reply("OK ENABLE %d", on ? 1 : 0);
    }
  } else if (keyword(command, "VERSION?") || keyword(command, "VERSION")) {
    if (args != 0) {
      reply("ERR BAD_ARG usage: VERSION?");
      return;
    }
    reply("OK VERSION %s hm_axis/%s", firmware_, kAxisLibVersion);
  } else if (keyword(command, "HELP") || keyword(command, "HELP?")) {
    reply("OK HELP HOME MOVE STOP ESTOP POS? STATUS? LIST SET ENABLE VERSION?");
  } else {
    char shown[17];
    strncpy(shown, command, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    reply("ERR UNKNOWN_COMMAND %s", shown);
  }
}

void Controller::doMove(char* argument) {
  toLower(argument);
  float mm = 0.0f;
  const PositionTable::Entry* entry = nullptr;
  if (!parseMm(argument, &mm)) {
    if (!isValidName(argument)) {
      reply("ERR BAD_ARG usage: MOVE <name|mm>");
      return;
    }
    entry = table_.find(argument);
    if (entry == nullptr) {
      reply("ERR UNKNOWN_POSITION %s", argument);
      return;
    }
    mm = entry->mm;
  }
  const AxisError allowed = axis_.canMove();
  if (allowed != AxisError::None) {
    replyError(allowed);
    return;
  }
  int32_t target = 0;
  if (!axis_.targetForMm(mm, &target)) {
    replyError(AxisError::OutOfRange);
    return;
  }
  const AxisError error = axis_.moveTo(target);
  if (error != AxisError::None) {
    replyError(error);
    return;
  }
  char target_mm[16];
  textForSteps(target, target_mm, sizeof(target_mm));
  if (entry != nullptr) {
    strncpy(move_label_, entry->name, sizeof(move_label_) - 1);
    move_label_[sizeof(move_label_) - 1] = '\0';
  } else {
    strncpy(move_label_, target_mm, sizeof(move_label_) - 1);
    move_label_[sizeof(move_label_) - 1] = '\0';
  }
  reply("OK MOVE %s %s", move_label_, target_mm);
}

void Controller::doSet(char* name, const char* value) {
  toLower(name);
  if (!isValidName(name)) {
    reply("ERR BAD_NAME use [a-z0-9_]{1,16}, not only digits");
    return;
  }
  float mm = 0.0f;
  if (!parseMm(value, &mm)) {
    reply("ERR BAD_ARG usage: SET <name> <mm>");
    return;
  }
  if (axis_.configProblem() != nullptr) {
    replyError(AxisError::ConfigInvalid);
    return;
  }
  if (!axis_.targetForMm(mm, nullptr)) {
    replyError(AxisError::OutOfRange);
    return;
  }
  if (table_.set(name, mm) == PositionTable::Result::Full) {
    reply("ERR TABLE_FULL max %u names", static_cast<unsigned>(kMaxNamedPositions));
    return;
  }
  char text[16];
  formatCentiMm(lroundf(mm * 100.0f), text, sizeof(text));
  reply("OK SET %s %s", name, text);
}

void Controller::doList() {
  size_t used = static_cast<size_t>(
      snprintf(output_, sizeof(output_), "OK LIST %u", static_cast<unsigned>(table_.count())));
  for (uint8_t index = 0; index < table_.count() && used < sizeof(output_); ++index) {
    char text[16];
    formatCentiMm(lroundf(table_.at(index).mm * 100.0f), text, sizeof(text));
    used += static_cast<size_t>(
        snprintf(output_ + used, sizeof(output_) - used, " %s=%s", table_.at(index).name, text));
  }
  sink_.writeLine(output_);
}

void Controller::poll() {
  AxisEvent event;
  while (axis_.takeEvent(&event)) {
    char mm[16];
    textForSteps(event.position, mm, sizeof(mm));
    switch (event.type) {
      case EventType::Homed:
        reply("EVT HOMED");
        break;
      case EventType::Arrived:
        reply("EVT ARRIVED %s %s", move_label_[0] != '\0' ? move_label_ : mm, mm);
        break;
      case EventType::Stopped:
        reply("EVT STOPPED %s", mm);
        break;
      case EventType::Error:
        if (event.error == AxisError::HomeFailed) {
          reply("EVT ERROR HOME_FAILED %s", failDetailName(event.detail));
        } else if (event.error == AxisError::EndstopHit) {
          reply("EVT ERROR ENDSTOP_HIT %s", mm);
        } else {
          reply("EVT ERROR %s", errorName(event.error));
        }
        break;
    }
  }
}

void Controller::replyError(AxisError error) {
  switch (error) {
    case AxisError::Busy:
      reply("ERR BUSY %s", stateName(axis_.state()));
      break;
    case AxisError::NotHomed:
      reply("ERR NOT_HOMED send HOME first");
      break;
    case AxisError::OutOfRange: {
      char travel[16];
      textForSteps(axis_.travelSteps(), travel, sizeof(travel));
      reply("ERR OUT_OF_RANGE allowed 0.00..%s", travel);
      break;
    }
    case AxisError::ConfigInvalid:
      reply("ERR CONFIG_INVALID %s", axis_.configProblem() != nullptr ? axis_.configProblem() : "-");
      break;
    default:
      reply("ERR %s -", errorName(error));
      break;
  }
}

void Controller::reply(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(output_, sizeof(output_), format, arguments);
  va_end(arguments);
  sink_.writeLine(output_);
}

void Controller::textForSteps(int32_t steps, char* out, size_t capacity) const {
  formatCentiMm(axis_.stepsToCentiMm(steps), out, capacity);
}

}  // namespace axis
}  // namespace hm
