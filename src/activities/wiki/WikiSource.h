#pragma once

#include <HalStorage.h>
#include <XwaArchive.h>

#include <cstdint>
#include <string>
#include <vector>

// The bytes of an archive on the SD card.
//
// SdFat finds a byte of a FAT file by walking the file's cluster chain, from
// the start whenever it seeks backwards: on a large archive that is hundreds
// of FAT sector reads for every jump between the index and the text. So on
// the device the archive is mapped once (which sectors hold each run of
// clusters), the map is kept in a small file next to the wiki's saved state,
// and reads go straight to the card's sectors. Before the map is used, the
// first and last sectors of the archive are read both ways and compared, and
// a map that disagrees is rebuilt or abandoned. Fragmented files work too:
// the archive need not be copied in one piece. Without a map (the simulator,
// or a card the map cannot describe) reads go through the file.
class WikiSource final : public xwa::Source {
 public:
  WikiSource() = default;
  ~WikiSource() override { close(); }
  WikiSource(const WikiSource&) = delete;
  WikiSource& operator=(const WikiSource&) = delete;

  // `mapPath` is where the cluster map is cached (rebuilt when stale).
  bool open(const std::string& path, const std::string& mapPath);
  void close();
  bool isOpen() const { return open_; }
  // True when reads go straight to the card's sectors.
  bool direct() const { return !extents_.empty(); }

  uint64_t size() override { return size_; }
  bool read(uint64_t offset, void* buf, size_t len) override;

 private:
  struct Extent {
    uint32_t fileSector;  // first 512-byte sector of the file this run covers
    uint32_t lba;         // where that sector is on the card
    uint32_t sectors;     // length of the run
  };

  bool readFile(uint64_t offset, void* buf, size_t len);
  bool readDirect(uint64_t offset, uint8_t* buf, size_t len);
  bool readSectors(uint32_t fileSector, uint8_t* buf, uint32_t count);
  bool mapFile(const std::string& path);
  bool loadMap(const std::string& mapPath, uint32_t stamp);
  void saveMap(const std::string& mapPath, uint32_t stamp) const;
  bool verifyMap();

  HalFile file_;
  bool open_ = false;
  uint64_t size_ = 0;
  uint32_t firstCluster_ = 0;
  std::vector<Extent> extents_;
  size_t lastExtent_ = 0;
  uint8_t* sector_ = nullptr;  // one sector for unaligned ends
};
