#pragma once

#include <XwaArchive.h>
#include <XwaFont.h>
#include <XwaMath.h>
#include <XwaSearch.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WikiSource.h"

// A place in an article: where a page starts, as a body position. It
// survives font and layout changes, unlike a page number.
struct WikiPlace {
  uint32_t article = xwa::NO_ARTICLE;
  uint32_t pos = 0;   // body position of the block a page starts in
  uint32_t line = 0;  // lines of that block on earlier pages
  bool valid() const { return article != xwa::NO_ARTICLE; }
};

// What the wiki remembers about one archive, in /.crosspoint/wiki/.
struct WikiMemory {
  static constexpr size_t MAX_TRAIL = 16;
  static constexpr size_t MAX_RECENT = 40;
  WikiPlace resume;              // where reading stopped
  std::vector<WikiPlace> trail;  // the articles Back returns to from there, oldest first
  std::vector<uint32_t> recent;  // articles read, newest first
};

// The archive every wiki screen reads, opened by the first screen that needs
// it and closed when the last one holding it goes away.
class WikiSession {
 public:
  ~WikiSession();
  WikiSession(const WikiSession&) = delete;
  WikiSession& operator=(const WikiSession&) = delete;

  // The open session, if a wiki screen holds one.
  static std::shared_ptr<WikiSession> current();
  // Opens `path` (or returns the open session when it is the same archive).
  static std::shared_ptr<WikiSession> open(const std::string& path, xwa::Status& status);

  xwa::Archive& archive() { return archive_; }
  xwa::TitleIndex& index() { return index_; }
  // MATH_GLYPHS, opened on first use; null when the archive has none.
  xwa::GlyphTable* glyphs();
  // FONT and its styled sections, opened on first use; null when the
  // archive has none.
  xwa::FallbackFont* fallback();
  const std::string& path() const { return path_; }
  // The archive's title (META), else its file name.
  const std::string& title() const { return title_; }
  bool fastReads() const { return source_.direct(); }

  WikiMemory& memory() { return memory_; }
  void saveMemory();
  // Puts an article at the top of the recent list.
  void noteRead(uint32_t article);
  // The article to show for "Main page", or NO_ARTICLE.
  uint32_t mainArticle() const { return mainArticle_; }

 private:
  WikiSession() : index_(archive_) {}
  void loadMemory();
  std::string stateBase() const;

  WikiSource source_;
  xwa::Archive archive_;
  xwa::TitleIndex index_;
  xwa::GlyphTable glyphs_;
  bool glyphsTried_ = false;
  xwa::FallbackFont fallback_;
  bool fallbackTried_ = false;
  std::string path_;
  std::string title_;
  uint32_t stamp_ = 0;
  uint32_t mainArticle_ = xwa::NO_ARTICLE;
  WikiMemory memory_;
};

namespace wiki {

// Where archives go (spec 3.4) and where the wiki keeps its own files.
constexpr const char* ARCHIVE_DIR = "/wiki";
constexpr const char* STATE_DIR = "/.crosspoint/wiki";

// Paths of the .xwa files in /wiki, sorted by name.
std::vector<std::string> listArchives();
// The archive opened last, if it still exists.
std::string lastArchive();
void setLastArchive(const std::string& path);
// A sentence for the user about an archive that did not open.
const char* statusMessage(xwa::Status status);
// The file name without folder or extension.
std::string baseName(const std::string& path);

}  // namespace wiki
