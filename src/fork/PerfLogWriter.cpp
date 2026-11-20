#include "PerfLogWriter.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PerfLog.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/CprVcodexLogs.h"
#include "version.h"

namespace perflogwriter {
namespace {

constexpr char TIMING_FILE[] = CPR_VCODEX_LOG_DIR "/timing.log";
// Let the screen settle after the last input so a write never lands inside the
// action being measured.
constexpr unsigned long IDLE_BEFORE_WRITE_MS = 2500;
constexpr size_t MAX_FILE_BYTES = 512 * 1024;
constexpr size_t BATCH = 16;

bool headerWritten = false;

void writeLine(HalFile& file, const char* line) { file.write(reinterpret_cast<const uint8_t*>(line), strlen(line)); }

}  // namespace

void tick(const unsigned long lastInputMs) {
  const bool wanted = SETTINGS.timingLog != 0;
  if (perflog::enabled() != wanted) perflog::setEnabled(wanted);
  if (!wanted || perflog::pending() == 0) return;
  if (millis() - lastInputMs < IDLE_BEFORE_WRITE_MS) return;

  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CPR_VCODEX_LOG_DIR);
  HalFile file = Storage.open(TIMING_FILE, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("PERF", "Could not open timing log");
    // Do not keep retrying every pass with a full buffer.
    perflog::Entry discard[BATCH];
    while (perflog::drain(discard, BATCH) > 0) {
    }
    return;
  }
  if (file.size() > MAX_FILE_BYTES) {
    file.close();
    Storage.remove(TIMING_FILE);
    file = Storage.open(TIMING_FILE, O_WRITE | O_CREAT | O_APPEND);
    if (!file) return;
    headerWritten = false;
  }

  char line[96];
  if (!headerWritten) {
    snprintf(line, sizeof(line), "# boot %s | columns: start_ms duration_ms event value\n", CROSSPOINT_VERSION);
    writeLine(file, line);
    headerWritten = true;
  }
  const uint32_t dropped = perflog::takeDropped();
  if (dropped > 0) {
    snprintf(line, sizeof(line), "# %lu events dropped (buffer full)\n", static_cast<unsigned long>(dropped));
    writeLine(file, line);
  }

  perflog::Entry batch[BATCH];
  size_t n = 0;
  while ((n = perflog::drain(batch, BATCH)) > 0) {
    for (size_t i = 0; i < n; ++i) {
      snprintf(line, sizeof(line), "%lu %lu %s %ld\n", static_cast<unsigned long>(batch[i].ms),
               static_cast<unsigned long>(batch[i].durationMs), batch[i].tag, static_cast<long>(batch[i].value));
      writeLine(file, line);
    }
  }
  file.flush();
  file.close();
}

}  // namespace perflogwriter
