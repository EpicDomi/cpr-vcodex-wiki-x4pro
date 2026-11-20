#include "WikiSession.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>

namespace {
constexpr const char* TAG = "WIKI";
constexpr uint32_t MEMORY_MAGIC = 0x53415758;  // "XWAS"
constexpr uint32_t MEMORY_VERSION = 2;
constexpr const char* LAST_ARCHIVE_FILE = "/.crosspoint/wiki/last.txt";

std::weak_ptr<WikiSession>& sessionSlot() {
  static std::weak_ptr<WikiSession> slot;
  return slot;
}

// The X4 has no PSRAM: a few cached blocks and one chunk. The X4 Pro keeps
// the upper levels of the title index and several chunks in PSRAM.
xwa::Archive::Options archiveOptions() {
  xwa::Archive::Options o;
#if defined(BOARD_HAS_PSRAM) || !defined(ESP_PLATFORM)
  o.cacheBlocks = 192;
  o.chunkSlots = 4;
#else
  o.cacheBlocks = 4;
  o.chunkSlots = 1;
#endif
  return o;
}

bool readU32(HalFile& f, uint32_t& v) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return false;
  v = xwa::le32(b);
  return true;
}

bool writeU32(HalFile& f, uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  return f.write(b, 4) == 4;
}
}  // namespace

WikiSession::~WikiSession() {
  archive_.close();
  source_.close();
}

std::shared_ptr<WikiSession> WikiSession::current() { return sessionSlot().lock(); }

std::shared_ptr<WikiSession> WikiSession::open(const std::string& path, xwa::Status& status) {
  if (auto existing = current()) {
    if (existing->path_ == path) {
      status = xwa::Status::Ok;
      return existing;
    }
  }
  std::shared_ptr<WikiSession> s(new (std::nothrow) WikiSession());
  if (!s) {
    status = xwa::Status::NoMemory;
    return nullptr;
  }
  Storage.mkdir(wiki::STATE_DIR);
  s->path_ = path;
  if (!s->source_.open(path, s->stateBase() + ".map")) {
    status = xwa::Status::ReadError;
    return nullptr;
  }
  status = s->archive_.open(&s->source_, archiveOptions());
  if (status != xwa::Status::Ok) {
    LOG_ERR(TAG, "%s: cannot open (status %d)", path.c_str(), static_cast<int>(status));
    return nullptr;
  }
  uint8_t stamp[4] = {};
  if (s->source_.read(4092, stamp, 4)) s->stamp_ = xwa::le32(stamp);
  s->title_ = s->archive_.metaValue("title");
  if (s->title_.empty()) s->title_ = wiki::baseName(path);
  const std::string& main = s->archive_.metaValue("main_article");
  if (!main.empty()) {
    char* end = nullptr;
    const unsigned long id = std::strtoul(main.c_str(), &end, 10);
    if (end && *end == '\0' && id < s->archive_.articleCount()) s->mainArticle_ = static_cast<uint32_t>(id);
  }
  s->loadMemory();
  sessionSlot() = s;
  wiki::setLastArchive(path);
  return s;
}

xwa::GlyphTable* WikiSession::glyphs() {
  if (!glyphsTried_) {
    glyphsTried_ = true;
    glyphs_.open(archive_);
  }
  return glyphs_.isOpen() ? &glyphs_ : nullptr;
}

xwa::FallbackFont* WikiSession::fallback() {
  if (!fallbackTried_) {
    fallbackTried_ = true;
    fallback_.open(archive_);
  }
  return fallback_.isOpen() ? &fallback_ : nullptr;
}

std::string WikiSession::stateBase() const { return std::string(wiki::STATE_DIR) + "/" + wiki::baseName(path_); }

void WikiSession::loadMemory() {
  memory_ = WikiMemory();
  HalFile f;
  if (!Storage.openFileForRead(TAG, stateBase() + ".state", f)) return;
  uint32_t magic = 0, version = 0, stamp = 0, count = 0;
  if (!readU32(f, magic) || !readU32(f, version) || !readU32(f, stamp)) return;
  // A state file from another archive of the same name is ignored.
  if (magic != MEMORY_MAGIC || version != MEMORY_VERSION || stamp != stamp_) return;
  const uint32_t articles = archive_.articleCount();
  WikiMemory m;
  if (!readU32(f, m.resume.article) || !readU32(f, m.resume.pos) || !readU32(f, m.resume.line)) return;
  if (m.resume.article >= articles) m.resume = WikiPlace();
  if (!readU32(f, count) || count > WikiMemory::MAX_TRAIL) return;
  for (uint32_t i = 0; i < count; i++) {
    WikiPlace p;
    if (!readU32(f, p.article) || !readU32(f, p.pos) || !readU32(f, p.line)) return;
    if (p.article < articles) m.trail.push_back(p);
  }
  if (!readU32(f, count) || count > WikiMemory::MAX_RECENT) return;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t a = 0;
    if (!readU32(f, a)) return;
    if (a < articles) m.recent.push_back(a);
  }
  memory_ = std::move(m);
}

void WikiSession::saveMemory() {
  const std::string path = stateBase() + ".state";
  const std::string tmp = path + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite(TAG, tmp, f)) return;
  bool ok = writeU32(f, MEMORY_MAGIC) && writeU32(f, MEMORY_VERSION) && writeU32(f, stamp_) &&
            writeU32(f, memory_.resume.article) && writeU32(f, memory_.resume.pos) && writeU32(f, memory_.resume.line);
  const size_t trail = std::min(memory_.trail.size(), WikiMemory::MAX_TRAIL);
  ok = ok && writeU32(f, static_cast<uint32_t>(trail));
  for (size_t i = memory_.trail.size() - trail; ok && i < memory_.trail.size(); i++) {
    const WikiPlace& p = memory_.trail[i];
    ok = writeU32(f, p.article) && writeU32(f, p.pos) && writeU32(f, p.line);
  }
  const size_t recent = std::min(memory_.recent.size(), WikiMemory::MAX_RECENT);
  ok = ok && writeU32(f, static_cast<uint32_t>(recent));
  for (size_t i = 0; ok && i < recent; i++) ok = writeU32(f, memory_.recent[i]);
  f.close();
  if (!ok || !Storage.replaceFile(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    LOG_ERR(TAG, "Could not save %s", path.c_str());
  }
}

void WikiSession::noteRead(uint32_t article) {
  auto& r = memory_.recent;
  r.erase(std::remove(r.begin(), r.end(), article), r.end());
  r.insert(r.begin(), article);
  if (r.size() > WikiMemory::MAX_RECENT) r.resize(WikiMemory::MAX_RECENT);
}

namespace wiki {

std::string baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.resize(dot);
  return name;
}

std::vector<std::string> listArchives() {
  std::vector<std::string> out;
  auto dir = Storage.open(ARCHIVE_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return out;
  }
  char name[256];
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    if (!f.isDirectory() && name[0] != '.' && FsHelpers::checkFileExtension(std::string_view(name), ".xwa")) {
      out.emplace_back(std::string(ARCHIVE_DIR) + "/" + name);
    }
    f.close();
  }
  dir.close();
  std::sort(out.begin(), out.end());
  return out;
}

std::string lastArchive() {
  std::string path;
  if (!Storage.readFileToString(TAG, LAST_ARCHIVE_FILE, 512, path)) return {};
  while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
  return !path.empty() && Storage.exists(path.c_str()) ? path : std::string();
}

void setLastArchive(const std::string& path) {
  if (lastArchive() == path) return;
  Storage.mkdir(STATE_DIR);
  Storage.writeFile(LAST_ARCHIVE_FILE, String(path.c_str()));
}

const char* statusMessage(xwa::Status status) {
  switch (status) {
    case xwa::Status::Ok:
      return "";
    case xwa::Status::ReadError:
      return tr(STR_WIKI_ERR_READ);
    case xwa::Status::NotXwa:
      return tr(STR_WIKI_ERR_NOT_XWA);
    case xwa::Status::Damaged:
      return tr(STR_WIKI_ERR_DAMAGED);
    case xwa::Status::Incomplete:
      return tr(STR_WIKI_ERR_INCOMPLETE);
    case xwa::Status::TooOld:
      return tr(STR_WIKI_ERR_TOO_OLD);
    case xwa::Status::TooNew:
      return tr(STR_WIKI_ERR_TOO_NEW);
    case xwa::Status::NeedsX4Pro:
      return tr(STR_WIKI_ERR_NEEDS_PRO);
    case xwa::Status::UnsupportedFeature:
      return tr(STR_WIKI_ERR_FEATURE);
    case xwa::Status::NoMemory:
      return tr(STR_WIKI_ERR_MEMORY);
  }
  return tr(STR_WIKI_ERR_READ);
}

}  // namespace wiki
