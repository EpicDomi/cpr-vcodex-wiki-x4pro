#include "WikiSource.h"

#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include <FS.h>  // before SdFat.h, as HalStorage.cpp does
#include <SDCardManager.h>
#endif

namespace {
constexpr const char* TAG = "WIKI";
constexpr uint32_t SECTOR = 512;
constexpr uint32_t MAP_MAGIC = 0x4D415758;  // "XWAM"
constexpr uint32_t MAP_VERSION = 1;
// A badly fragmented archive could need a run per cluster; past this many
// runs the map costs more RAM than it saves, and reads go through the file.
#if defined(BOARD_HAS_PSRAM)
constexpr size_t MAX_EXTENTS = 32768;
#else
constexpr size_t MAX_EXTENTS = 1024;
#endif

#if defined(ESP_PLATFORM)
// The card's sector device. SDMMC boards (X4 Pro), and SPI builds with the
// block-device interface, hand it out. The other SPI builds (X4) keep SdFat's
// card object private in SDCardManager; it is reached through a member
// pointer formed by an explicit template instantiation, which C++ exempts
// from access checks. Whatever the device, verifyMap() compares what it
// reads with the file before any of it is used.
#if FREEINK_SD_SDMMC || USE_BLOCK_DEVICE_INTERFACE
using RawDevice = FsBlockDeviceInterface;
RawDevice* rawDevice() { return SdMan.rawBlockDevice(); }
#else
using RawDevice = SdCard;
struct SdMember {
  using type = SdFat SDCardManager::*;
};
SdMember::type sdMember(SdMember);
template <typename Tag, typename Tag::type M>
struct Expose {
  friend typename Tag::type sdMember(Tag) { return M; }
};
template struct Expose<SdMember, &SDCardManager::sd>;
RawDevice* rawDevice() { return SdMan.ready() ? (SdMan.*sdMember(SdMember{})).card() : nullptr; }
#endif
#endif
}  // namespace

bool WikiSource::open(const std::string& path, const std::string& mapPath) {
  close();
  if (!Storage.openFileForRead(TAG, path, file_)) return false;
  size_ = file_.fileSize64();
  open_ = true;
#if defined(ESP_PLATFORM)
  // The archive's own superblock CRC tells one archive from another, so a
  // map is never applied to a different file that happens to have its name.
  uint8_t stampBytes[4] = {};
  if (size_ < 4096 || !readFile(4092, stampBytes, 4)) return true;
  const uint32_t stamp = xwa::le32(stampBytes);
  sector_ = static_cast<uint8_t*>(std::malloc(SECTOR));
  if (!sector_) return true;
  {
    HalStorage::RawAccessLock lock;
    FsFile f;
    if (!SdMan.openFileForRead(TAG, path.c_str(), f)) return true;
    firstCluster_ = f.firstCluster();
  }
  if (loadMap(mapPath, stamp) && verifyMap()) {
    LOG_INF(TAG, "%s: direct reads, %u runs (saved map)", path.c_str(), static_cast<unsigned>(extents_.size()));
    return true;
  }
  extents_.clear();
  const uint32_t started = millis();
  if (mapFile(path) && verifyMap()) {
    saveMap(mapPath, stamp);
    LOG_INF(TAG, "%s: direct reads, %u runs (mapped in %lu ms)", path.c_str(), static_cast<unsigned>(extents_.size()),
            static_cast<unsigned long>(millis() - started));
    return true;
  }
  extents_.clear();
  LOG_ERR(TAG, "%s: reading through the file (no usable sector map)", path.c_str());
#endif
  return true;
}

void WikiSource::close() {
  if (file_.isOpen()) file_.close();
  extents_.clear();
  extents_.shrink_to_fit();
  std::free(sector_);
  sector_ = nullptr;
  open_ = false;
  size_ = 0;
  firstCluster_ = 0;
  lastExtent_ = 0;
}

bool WikiSource::read(uint64_t offset, void* buf, size_t len) {
  if (!open_ || offset > size_ || len > size_ - offset) return false;
  if (len == 0) return true;
  if (!extents_.empty()) return readDirect(offset, static_cast<uint8_t*>(buf), len);
  return readFile(offset, buf, len);
}

bool WikiSource::readFile(uint64_t offset, void* buf, size_t len) {
  auto* out = static_cast<uint8_t*>(buf);
  if (!file_.seek64(offset)) return false;
  while (len > 0) {
    const size_t n = std::min<size_t>(len, 32768);
    if (file_.read(out, n) != static_cast<int>(n)) return false;
    out += n;
    len -= n;
  }
  return true;
}

bool WikiSource::readDirect(uint64_t offset, uint8_t* buf, size_t len) {
  while (len > 0) {
    const uint32_t sector = static_cast<uint32_t>(offset / SECTOR);
    const uint32_t within = static_cast<uint32_t>(offset % SECTOR);
    if (within == 0 && len >= SECTOR) {
      const uint32_t count = static_cast<uint32_t>(len / SECTOR);
      if (!readSectors(sector, buf, count)) return false;
      const size_t n = static_cast<size_t>(count) * SECTOR;
      buf += n;
      offset += n;
      len -= n;
      continue;
    }
    // A partial sector at either end of the read.
    if (!readSectors(sector, sector_, 1)) return false;
    const size_t n = std::min<size_t>(len, SECTOR - within);
    std::memcpy(buf, sector_ + within, n);
    buf += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool WikiSource::readSectors(uint32_t fileSector, uint8_t* buf, uint32_t count) {
#if defined(ESP_PLATFORM)
  HalStorage::RawAccessLock lock;
  RawDevice* dev = rawDevice();
  if (!dev) return false;
  while (count > 0) {
    // The run holding fileSector: usually the one used last.
    size_t i = lastExtent_ < extents_.size() ? lastExtent_ : 0;
    const Extent* e = &extents_[i];
    if (fileSector < e->fileSector || fileSector >= e->fileSector + e->sectors) {
      const auto it = std::upper_bound(extents_.begin(), extents_.end(), fileSector,
                                       [](uint32_t s, const Extent& x) { return s < x.fileSector; });
      if (it == extents_.begin()) return false;
      i = static_cast<size_t>(it - extents_.begin()) - 1;
      e = &extents_[i];
      if (fileSector >= e->fileSector + e->sectors) return false;
    }
    lastExtent_ = i;
    const uint32_t n = std::min(count, e->fileSector + e->sectors - fileSector);
    const uint32_t lba = e->lba + (fileSector - e->fileSector);
    if (!dev->readSectors(lba, buf, n)) {
      // One retry after letting the card finish whatever it was doing.
      dev->syncDevice();
      if (!dev->readSectors(lba, buf, n)) return false;
    }
    buf += static_cast<size_t>(n) * SECTOR;
    fileSector += n;
    count -= n;
  }
  return true;
#else
  (void)fileSector;
  (void)buf;
  (void)count;
  return false;
#endif
}

bool WikiSource::mapFile(const std::string& path) {
#if defined(ESP_PLATFORM)
  HalStorage::RawAccessLock lock;
  if (!rawDevice()) return false;
  FsFile f;
  if (!SdMan.openFileForRead(TAG, path.c_str(), f)) return false;
  firstCluster_ = f.firstCluster();
  const uint32_t firstSector = static_cast<uint32_t>(f.firstSector());
  if (firstCluster_ < 2 || firstSector == 0) return false;
  // The cluster size, from the file itself: the first power of two at which
  // its cluster changes (clusters are 512 B to 32 MB, powers of two).
  if (!f.seekSet(1)) return false;
  const uint32_t cluster0 = f.curCluster();
  uint64_t clusterBytes = 0;
  for (uint64_t bytes = SECTOR; bytes <= (1ull << 25) && bytes < size_; bytes <<= 1) {
    if (!f.seekSet(bytes + 1)) return false;
    if (f.curCluster() != cluster0) {
      clusterBytes = bytes;
      break;
    }
  }
  extents_.clear();
  if (clusterBytes == 0) {
    // All in one cluster.
    extents_.push_back(Extent{0, firstSector, static_cast<uint32_t>((size_ + SECTOR - 1) / SECTOR)});
    return true;
  }
  const uint32_t clusterSectors = static_cast<uint32_t>(clusterBytes / SECTOR);
  const uint64_t clusters = (size_ + clusterBytes - 1) / clusterBytes;
  if (clusters * clusterSectors > 0xFFFFFFFFull) return false;
  // Sector of cluster c, from where the first cluster is.
  const auto lbaOf = [&](uint32_t cluster) {
    return static_cast<uint32_t>(firstSector + (static_cast<int64_t>(cluster) - firstCluster_) * clusterSectors);
  };
  if (f.isContiguous()) {
    extents_.push_back(Extent{0, firstSector, static_cast<uint32_t>(clusters * clusterSectors)});
    return true;
  }
  // Follow the cluster chain once: each forward seek of one cluster is one
  // FAT entry, read through SdFat's FAT cache.
  uint32_t previous = 0;
  for (uint64_t i = 0; i < clusters; i++) {
    if (!f.seekSet(i * clusterBytes + 1)) return false;
    const uint32_t cluster = f.curCluster();
    if (cluster < 2) return false;
    if (!extents_.empty() && cluster == previous + 1) {
      extents_.back().sectors += clusterSectors;
    } else {
      if (extents_.size() >= MAX_EXTENTS) {
        LOG_ERR(TAG, "Archive is in more than %u pieces; copying it again to a freshly formatted card speeds it up",
                static_cast<unsigned>(MAX_EXTENTS));
        extents_.clear();
        return false;
      }
      extents_.push_back(Extent{static_cast<uint32_t>(i * clusterSectors), lbaOf(cluster), clusterSectors});
    }
    previous = cluster;
    if ((i & 4095) == 4095) delay(1);  // let other tasks run during a long walk
  }
  return true;
#else
  (void)path;
  return false;
#endif
}

bool WikiSource::verifyMap() {
  if (extents_.empty()) return false;
  // The first and last 4 KB, read both ways, must agree.
  const size_t probe = static_cast<size_t>(std::min<uint64_t>(size_, 4096));
  uint8_t* a = static_cast<uint8_t*>(std::malloc(probe));
  uint8_t* b = static_cast<uint8_t*>(std::malloc(probe));
  bool ok = a && b;
  for (const uint64_t at : {uint64_t{0}, size_ - probe}) {
    if (!ok) break;
    ok = readDirect(at, a, probe) && readFile(at, b, probe) && std::memcmp(a, b, probe) == 0;
  }
  std::free(a);
  std::free(b);
  if (!ok) LOG_ERR(TAG, "Sector map does not match the file");
  return ok;
}

bool WikiSource::loadMap(const std::string& mapPath, uint32_t stamp) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, mapPath, f)) return false;
  uint8_t head[28];
  if (f.read(head, sizeof(head)) != static_cast<int>(sizeof(head))) return false;
  const uint32_t count = xwa::le32(head + 24);
  if (xwa::le32(head) != MAP_MAGIC || xwa::le32(head + 4) != MAP_VERSION || xwa::le64(head + 8) != size_ ||
      xwa::le32(head + 16) != firstCluster_ || xwa::le32(head + 20) != stamp || count == 0 || count > MAX_EXTENTS) {
    return false;
  }
  extents_.resize(count);
  uint32_t expect = 0;
  for (auto& e : extents_) {
    uint8_t r[12];
    if (f.read(r, sizeof(r)) != static_cast<int>(sizeof(r))) return false;
    e = Extent{xwa::le32(r), xwa::le32(r + 4), xwa::le32(r + 8)};
    if (e.fileSector != expect || e.sectors == 0) return false;
    expect += e.sectors;
  }
  return static_cast<uint64_t>(expect) * SECTOR >= size_;
}

void WikiSource::saveMap(const std::string& mapPath, uint32_t stamp) const {
  const std::string tmp = mapPath + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite(TAG, tmp, f)) return;
  auto put32 = [&](uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                    static_cast<uint8_t>(v >> 24)};
    return f.write(b, 4) == 4;
  };
  bool ok = put32(MAP_MAGIC) && put32(MAP_VERSION) && put32(static_cast<uint32_t>(size_)) &&
            put32(static_cast<uint32_t>(size_ >> 32)) && put32(firstCluster_) && put32(stamp) &&
            put32(static_cast<uint32_t>(extents_.size()));
  for (const auto& e : extents_) ok = ok && put32(e.fileSector) && put32(e.lba) && put32(e.sectors);
  f.close();
  if (!ok || !Storage.replaceFile(tmp.c_str(), mapPath.c_str())) Storage.remove(tmp.c_str());
}
