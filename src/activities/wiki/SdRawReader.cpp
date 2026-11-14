#include "SdRawReader.h"

#include <cstring>
#include <new>

#include "WikiShared.h"
#ifdef ESP_PLATFORM
#include <BoardConfig.h>
#include <HalStorage.h>

// Block-device access differs per SD transport:
//  * Native SDMMC (Xteink X4 Pro, ESP32-S3): SDCardManager::rawBlockDevice() hands back the
//    card as an FsBlockDeviceInterface.
//  * SPI (Xteink X4/X3, ESP32-C3): rawBlockDevice() only answers when the build sets
//    USE_BLOCK_DEVICE_INTERFACE, which the C3 envs do not (it is tied to USB Drive), so the
//    private SdFat `sd` member is reached the same way upstream's wiki port did.
#if FREEINK_SD_SDMMC
#include <SDCardManager.h>
#else
#define private public
#include <SDCardManager.h>
#undef private
#endif
#include <Logging.h>

static const char* TAG = "SdRawReader";

namespace {
// SdFat's SPI card only derives from FsBlockDeviceInterface when the build sets
// USE_BLOCK_DEVICE_INTERFACE (the SDMMC envs do, the SPI envs do not), so name the
// concrete type per transport. Both offer readSectors() and syncDevice().
#if FREEINK_SD_SDMMC
using WikiBlockDevice = FsBlockDeviceInterface;
WikiBlockDevice* wikiBlockDevice() { return SDCardManager::getInstance().rawBlockDevice(); }
#else
using WikiBlockDevice = SdCard;
WikiBlockDevice* wikiBlockDevice() { return SDCardManager::getInstance().sd.card(); }
#endif

// FAT16/32 has no on-disk "contiguous" flag (exFAT does), so SdFat only reports a FAT file as
// contiguous after walking its whole cluster chain. That walk costs up to ~1000 sector reads
// on a 4 GB file, and every wiki screen mounts its own reader, so remember the last answer.
struct ContiguityCache {
  bool valid = false;
  bool contiguous = false;
  uint32_t firstSector = 0;
  uint64_t fileSize = 0;
};
ContiguityCache contiguityCache;

bool fileIsContiguous(FsFile& file, const uint32_t firstSector, const uint64_t fileSize) {
  if (file.isContiguous()) {
    return true;
  }
  if (contiguityCache.valid && contiguityCache.firstSector == firstSector && contiguityCache.fileSize == fileSize) {
    return contiguityCache.contiguous;
  }
  Sector_t bgn = 0;
  Sector_t end = 0;
  const bool contiguous = file.contiguousRange(&bgn, &end) && bgn == firstSector;
  contiguityCache.valid = true;
  contiguityCache.contiguous = contiguous;
  contiguityCache.firstSector = firstSector;
  contiguityCache.fileSize = fileSize;
  return contiguous;
}
}  // namespace

bool SdRawReader::mount(int, int, int, int) {
  if (mounted_) {
    return true;
  }

  // Raw sector reads bypass HalFile, so they must take the storage lock themselves: the render
  // task and the main loop both read the card, and on the dual-core X4 Pro the SDMMC driver
  // funnels every transfer through one shared bounce buffer.
  HalStorage::RawAccessLock lock;

  if (!SDCardManager::getInstance().ready()) {
    LOG_ERR(TAG, "SDCardManager not ready for raw reads");
    return false;
  }

  if (!wikiBlockDevice()) {
    LOG_ERR(TAG, "rawBlockDevice unavailable from SDCardManager");
    return false;
  }

  const char* imgPath = WikiConfig::get().imagePath;
  if (imgPath[0] == '\0') {
    return false;
  }

  FsFile* file = new (std::nothrow) FsFile();
  if (!file) {
    LOG_ERR(TAG, "Out of memory opening %s", imgPath);
    return false;
  }
  if (!SDCardManager::getInstance().openFileForRead(TAG, imgPath, *file)) {
    LOG_ERR(TAG, "Failed to open %s", imgPath);
    delete file;
    return false;
  }

  const uint64_t fileSize = file->fileSize();
  const uint32_t firstSector = static_cast<uint32_t>(file->firstSector());
  sector_count_ = static_cast<uint32_t>(fileSize / 512);
  if (firstSector != 0 && fileIsContiguous(*file, firstSector, fileSize)) {
    base_sector_ = firstSector;
    is_file_ = false;
    file->close();
    delete file;
    file_ptr_ = nullptr;
    LOG_INF(TAG, "Mounted %s: contiguous, raw reads at LBA %u (%u sectors)", imgPath,
            static_cast<unsigned>(base_sector_), static_cast<unsigned>(sector_count_));
  } else {
    base_sector_ = 0;
    is_file_ = true;
    file_ptr_ = file;
    LOG_ERR(TAG,
            "Mounted %s: File IS NOT contiguous! SD reads will be extremely slow. Please re-copy to a freshly "
            "formatted card.",
            imgPath);
  }

  mounted_ = true;
  return true;
}

bool SdRawReader::readSectors(size_t startSector, size_t sectorCount, uint8_t* buffer) {
  if (!mounted_ || !buffer || sectorCount == 0) {
    return false;
  }

  if (startSector >= sector_count_ || sectorCount > sector_count_ - startSector) {
    return false;
  }

  HalStorage::RawAccessLock lock;

  if (is_file_) {
    if (!file_ptr_) return false;
    FsFile* file = static_cast<FsFile*>(file_ptr_);
    if (!file->seekSet(static_cast<uint64_t>(startSector) * 512)) return false;
    return file->read(buffer, sectorCount * 512) == static_cast<int>(sectorCount * 512);
  }

  WikiBlockDevice* blockDev = wikiBlockDevice();
  if (!blockDev) {
    return false;
  }

  const uint32_t lba = static_cast<uint32_t>(startSector) + base_sector_;
  if (blockDev->readSectors(lba, buffer, sectorCount)) {
    return true;
  }
  blockDev->syncDevice();
  return blockDev->readSectors(lba, buffer, sectorCount);
}

void SdRawReader::unmount() {
  if (mounted_) {
    if (file_ptr_) {
      HalStorage::RawAccessLock lock;
      FsFile* file = static_cast<FsFile*>(file_ptr_);
      file->close();
      delete file;
      file_ptr_ = nullptr;
    }
    mounted_ = false;
    LOG_INF(TAG, "Unmounted raw block device");
  }
}

#else
#include <cstdio>
#include <cstdlib>
#include <iostream>

bool SdRawReader::mount(int, int, int, int) {
  if (mounted_) return true;

  const char* envPath = std::getenv("WIKI_RAW_DRIVE");
  if (!envPath || !*envPath) envPath = std::getenv("CROSSINK_RAW_DRIVE");

  if (envPath && *envPath) {
    raw_drive_ = fopen(envPath, "rb");
    if (raw_drive_) {
      mounted_ = true;
      fseeko(raw_drive_, 0, SEEK_END); sector_count_ = ftello(raw_drive_) / 512; fseeko(raw_drive_, 0, SEEK_SET);
      std::cout << "[SdRawReader] Mounted raw drive from env: " << envPath << "\n";
      return true;
    }
  }

  const char* cfgPath = WikiConfig::get().imagePath;
  if (cfgPath[0] != '\0') {
      std::string path = cfgPath;
      if (path.length() > 0 && path[0] == '/') {
          path = "fs_" + path;
      }
      raw_drive_ = fopen(path.c_str(), "rb");
      if (raw_drive_) {
          fseeko(raw_drive_, 0, SEEK_END);
          sector_count_ = ftello(raw_drive_) / 512;
          fseeko(raw_drive_, 0, SEEK_SET);
          mounted_ = true;
          std::cout << "[SdRawReader] Mounted raw image from settings: " << path << " (sectors: " << sector_count_ << ")\n";
          return true;
      }
  }

  const char* candidatePaths[] = {
      "sdcard.img",
      "../sdcard.img",
      "fs_/sdcard.img",
  };
  for (const char* path : candidatePaths) {
    FILE* f = fopen(path, "rb");
    if (f) {
      raw_drive_ = f;
      mounted_ = true;
      fseeko(raw_drive_, 0, SEEK_END); sector_count_ = ftello(raw_drive_) / 512; fseeko(raw_drive_, 0, SEEK_SET);
      std::cout << "[SdRawReader] Mounted raw image: " << path << "\n";
      return true;
    }
  }

  index_file_ = fopen("../wiki_indexer/index.bin", "rb");
  chunks_file_ = fopen("../wiki_indexer/chunks_0.gz", "rb");
  if (!index_file_) index_file_ = fopen("wiki_indexer/index.bin", "rb");
  if (!chunks_file_) chunks_file_ = fopen("wiki_indexer/chunks_0.gz", "rb");

  if (index_file_) {
    fseeko(index_file_, 0, SEEK_END);
    off_t index_bytes = ftello(index_file_);
    index_sectors_ = (index_bytes + 511) / 512;
  }

  mounted_ = (index_file_ != nullptr && chunks_file_ != nullptr);
  if (mounted_) {
    std::cout << "[SdRawReader] Mounted mock split files (index.bin + chunks_0.gz)\n";
  }
  return mounted_;
}

bool SdRawReader::readSectors(size_t startSector, size_t sectorCount, uint8_t* buffer) {
  if (!mounted_ || !buffer || sectorCount == 0) return false;

  if (raw_drive_) {
    off_t byteOffset = static_cast<off_t>(startSector) * 512;
    if (fseeko(raw_drive_, byteOffset, SEEK_SET) != 0) return false;
    size_t bytes = fread(buffer, 1, sectorCount * 512, raw_drive_);
    if (bytes < sectorCount * 512) {
      std::memset(buffer + bytes, 0, sectorCount * 512 - bytes);
    }
    return bytes > 0;
  }

  if (index_file_ && chunks_file_) {
    for (size_t s = 0; s < sectorCount; ++s) {
      size_t sec = startSector + s;
      uint8_t* secBuf = buffer + s * 512;
      if (sec < index_sectors_) {
        fseeko(index_file_, static_cast<off_t>(sec) * 512, SEEK_SET);
        size_t rd = fread(secBuf, 1, 512, index_file_);
        if (rd < 512) std::memset(secBuf + rd, 0, 512 - rd);
      } else {
        size_t chunkSec = sec - index_sectors_;
        fseeko(chunks_file_, static_cast<off_t>(chunkSec) * 512, SEEK_SET);
        size_t rd = fread(secBuf, 1, 512, chunks_file_);
        if (rd < 512) std::memset(secBuf + rd, 0, 512 - rd);
      }
    }
    return true;
  }

  return false;
}

void SdRawReader::unmount() {
  if (!mounted_) return;
  if (raw_drive_) {
    fclose(raw_drive_);
    raw_drive_ = nullptr;
  }
  if (index_file_) {
    fclose(index_file_);
    index_file_ = nullptr;
  }
  if (chunks_file_) {
    fclose(chunks_file_);
    chunks_file_ = nullptr;
  }
  mounted_ = false;
}
#endif
