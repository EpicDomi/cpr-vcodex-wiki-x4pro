#include "PerfLog.h"

#include <Arduino.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace perflog {
namespace {

constexpr size_t CAPACITY = 128;
constexpr uint32_t INPUT_MARK_INTERVAL_MS = 250;

std::mutex bufferMutex;
Entry entries[CAPACITY];
size_t head = 0;  // index of the oldest entry
size_t count = 0;
uint32_t dropped = 0;
uint32_t lastInputMarkMs = 0;
std::atomic<bool> recording{false};

void push(const char* tag, const uint32_t startMs, const uint32_t durationMs, const int32_t value) {
  std::lock_guard<std::mutex> lock(bufferMutex);
  if (count == CAPACITY) {
    ++dropped;
    return;
  }
  Entry& entry = entries[(head + count) % CAPACITY];
  entry.ms = startMs;
  entry.durationMs = durationMs;
  entry.value = value;
  std::snprintf(entry.tag, sizeof(entry.tag), "%s", tag ? tag : "");
  ++count;
}

}  // namespace

void setEnabled(const bool enabled) { recording.store(enabled, std::memory_order_relaxed); }

bool enabled() { return recording.load(std::memory_order_relaxed); }

void mark(const char* tag, const int32_t value) {
  if (!enabled()) return;
  push(tag, millis(), 0, value);
}

void markNamed(const char* prefix, const char* name) {
  if (!enabled()) return;
  char tag[sizeof(Entry::tag)];
  std::snprintf(tag, sizeof(tag), "%s%s", prefix ? prefix : "", name ? name : "");
  push(tag, millis(), 0, 0);
}

void markInput() {
  if (!enabled()) return;
  const uint32_t now = millis();
  if (lastInputMarkMs != 0 && now - lastInputMarkMs < INPUT_MARK_INTERVAL_MS) return;
  lastInputMarkMs = now;
  push("input", now, 0, 0);
}

Scope::Scope(const char* tag, const int32_t value) : tag_(tag), value_(value), startMs_(0), active_(enabled()) {
  if (active_) startMs_ = millis();
}

Scope::~Scope() {
  if (!active_) return;
  push(tag_, startMs_, millis() - startMs_, value_);
}

size_t pending() {
  std::lock_guard<std::mutex> lock(bufferMutex);
  return count;
}

uint32_t takeDropped() {
  std::lock_guard<std::mutex> lock(bufferMutex);
  const uint32_t result = dropped;
  dropped = 0;
  return result;
}

size_t drain(Entry* out, const size_t max) {
  if (out == nullptr || max == 0) return 0;
  std::lock_guard<std::mutex> lock(bufferMutex);
  size_t moved = 0;
  while (moved < max && count > 0) {
    out[moved++] = entries[head];
    head = (head + 1) % CAPACITY;
    --count;
  }
  return moved;
}

}  // namespace perflog
