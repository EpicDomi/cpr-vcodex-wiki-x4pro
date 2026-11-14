#pragma once

#include <cstddef>
#include <cstdint>

// Fork diagnostic: a small in-RAM timeline of UI events (input, activity
// enter/exit, renders, panel refreshes). Recording is a no-op until
// setEnabled(true); the firmware drains the entries to the SD card while idle
// (src/fork/PerfLogWriter). Safe to call from the main and render tasks.
namespace perflog {

struct Entry {
  uint32_t ms;          // millis() when the event started
  uint32_t durationMs;  // 0 for instant marks
  int32_t value;
  char tag[20];
};

void setEnabled(bool enabled);
bool enabled();

// Instant event.
void mark(const char* tag, int32_t value = 0);
// Instant event tagged "<prefix><name>", truncated to fit.
void markNamed(const char* prefix, const char* name);
// Instant "input" event, at most one every 250 ms.
void markInput();

// Records the tag with the elapsed time when it goes out of scope.
class Scope {
 public:
  explicit Scope(const char* tag, int32_t value = 0);
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  const char* tag_;
  int32_t value_;
  uint32_t startMs_;
  bool active_;
};

size_t pending();
// Entries lost because the buffer was full; reading resets the count.
uint32_t takeDropped();
// Moves up to max entries (oldest first) into out and returns how many.
size_t drain(Entry* out, size_t max);

}  // namespace perflog
