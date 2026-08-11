#pragma once
// Host-testable SD boot status classification for Murphy M4.
// Maps low-level mount outcomes to user-visible stages without formatting media.

#include <cstdint>
#include <cstring>
#include <string>

namespace M4SdStatus {

enum class Stage : uint8_t {
  Idle = 0,
  PowerCycle,
  HostInit,
  CardInit,
  SectorProbe,
  VolumeMount,
  CapabilityProbe,
  Ready,
};

enum class Code : uint8_t {
  Ok = 0,
  NoCard,               // card_init failed / no CSD
  HostInitFailed,       // sdmmc_host_init / slot
  MountTimeout,         // retries exhausted on block I/O
  UnsupportedFilesystem,// volume begin failed on all parts
  IoFailure,            // post-mount open/read/seek failed
  NotInitialized,
};

struct Result {
  Code code = Code::NotInitialized;
  Stage stage = Stage::Idle;
  int attempt = 0;
  int mountedPart = -1;  // 0=superfloppy, 1..4 MBR
  uint8_t fatType = 0;
  uint64_t sectorCount = 0;
  char detail[96] = {};
};

inline const char* codeName(Code c) {
  switch (c) {
    case Code::Ok: return "ok";
    case Code::NoCard: return "no_card";
    case Code::HostInitFailed: return "host_init_failed";
    case Code::MountTimeout: return "mount_timeout";
    case Code::UnsupportedFilesystem: return "unsupported_fs";
    case Code::IoFailure: return "io_failure";
    case Code::NotInitialized: return "not_initialized";
  }
  return "unknown";
}

inline const char* stageName(Stage s) {
  switch (s) {
    case Stage::Idle: return "idle";
    case Stage::PowerCycle: return "power_cycle";
    case Stage::HostInit: return "host_init";
    case Stage::CardInit: return "card_init";
    case Stage::SectorProbe: return "sector_probe";
    case Stage::VolumeMount: return "volume_mount";
    case Stage::CapabilityProbe: return "capability_probe";
    case Stage::Ready: return "ready";
  }
  return "unknown";
}

// Bounded recovery policy (no format/repartition).
struct RetryPolicy {
  int maxAttempts = 4;
  int powerHighMs = 80;
  int powerLowMs = 120;
};

inline bool shouldRetry(const RetryPolicy& p, int attempt, Code lastCode) {
  if (attempt + 1 >= p.maxAttempts) return false;
  // Retry hardware/transient paths only; never loop on unsupported FS forever.
  return lastCode == Code::NoCard || lastCode == Code::MountTimeout || lastCode == Code::HostInitFailed ||
         lastCode == Code::IoFailure;
}

// User-facing short message (English for serial; UI may translate).
inline const char* userMessage(Code c) {
  switch (c) {
    case Code::Ok: return "SD ready";
    case Code::NoCard: return "SD: no card detected";
    case Code::HostInitFailed: return "SD: host/slot init failed";
    case Code::MountTimeout: return "SD: mount timed out (block I/O)";
    case Code::UnsupportedFilesystem: return "SD: unsupported or missing FAT volume";
    case Code::IoFailure: return "SD: I/O failed after mount";
    case Code::NotInitialized: return "SD: not initialized";
  }
  return "SD: error";
}

inline void setDetail(Result& r, const char* msg) {
  if (!msg) {
    r.detail[0] = 0;
    return;
  }
  std::strncpy(r.detail, msg, sizeof(r.detail) - 1);
  r.detail[sizeof(r.detail) - 1] = 0;
}

// Classify volume-mount outcomes after block device is live.
inline Code classifyVolumeMount(bool anyPartMounted) {
  return anyPartMounted ? Code::Ok : Code::UnsupportedFilesystem;
}

// Classify post-mount capability probe (list root / open existing file).
inline Code classifyCapabilityProbe(bool rootListOk, bool optionalFileReadOk, bool hadOptionalFile) {
  if (!rootListOk) return Code::IoFailure;
  if (hadOptionalFile && !optionalFileReadOk) return Code::IoFailure;
  return Code::Ok;
}

// Structured block-device outcomes (SdmmcBlockDevice).
enum class BlockStage : uint8_t {
  Idle = 0,
  HostInit,
  SlotInit,
  CardInit,
  SectorRead,
  Ready,
};

enum class BlockCode : uint8_t {
  Ok = 0,
  HostInitFailed,
  SlotInitFailed,
  NoCard,         // card_init failed / no CSD
  SectorTimeout,  // read sector 0 timed out / ESP timeout
  SectorIoError,  // other read failure
  Oom,
};

inline const char* blockStageName(BlockStage s) {
  switch (s) {
    case BlockStage::Idle: return "idle";
    case BlockStage::HostInit: return "host_init";
    case BlockStage::SlotInit: return "slot_init";
    case BlockStage::CardInit: return "card_init";
    case BlockStage::SectorRead: return "sector_read";
    case BlockStage::Ready: return "ready";
  }
  return "unknown";
}

inline const char* blockCodeName(BlockCode c) {
  switch (c) {
    case BlockCode::Ok: return "ok";
    case BlockCode::HostInitFailed: return "host_init_failed";
    case BlockCode::SlotInitFailed: return "slot_init_failed";
    case BlockCode::NoCard: return "no_card";
    case BlockCode::SectorTimeout: return "sector_timeout";
    case BlockCode::SectorIoError: return "sector_io_error";
    case BlockCode::Oom: return "oom";
  }
  return "unknown";
}

struct BlockResult {
  BlockCode code = BlockCode::Ok;
  BlockStage stage = BlockStage::Idle;
  int attempt = 0;
  int espErr = 0;
  char detail[64] = {};
};

// Map block-device result into mount Result used by SDCardManager/main.
inline Result fromBlockResult(const BlockResult& b) {
  Result r;
  r.attempt = b.attempt;
  setDetail(r, b.detail);
  switch (b.code) {
    case BlockCode::Ok:
      r.code = Code::Ok;
      r.stage = Stage::SectorProbe;
      break;
    case BlockCode::HostInitFailed:
    case BlockCode::SlotInitFailed:
      r.code = Code::HostInitFailed;
      r.stage = Stage::HostInit;
      break;
    case BlockCode::NoCard:
      r.code = Code::NoCard;
      r.stage = Stage::CardInit;
      break;
    case BlockCode::SectorTimeout:
      r.code = Code::MountTimeout;
      r.stage = Stage::SectorProbe;
      break;
    case BlockCode::SectorIoError:
    case BlockCode::Oom:
      r.code = Code::IoFailure;
      r.stage = Stage::SectorProbe;
      break;
  }
  return r;
}

// Production mapping seam used by SDCardManager (and host tests).
// Integer codes MUST match freeink::SdmmcFailCode / SdmmcFailStage enum order in
// open-m4-sdk/.../SdmmcBlockDevice.h (Ok=0, HostInitFailed=1, ...).
struct ProductionMapOut {
  const char* stage = "unknown";
  const char* code = "unknown";
};

inline ProductionMapOut mapSdmmcFailToReport(int failCode, int /*failStage*/) {
  ProductionMapOut o;
  switch (static_cast<BlockCode>(failCode)) {
    case BlockCode::Ok:
      o.stage = "ready";
      o.code = "ok";
      break;
    case BlockCode::HostInitFailed:
      o.stage = "host_init";
      o.code = "host_init_failed";
      break;
    case BlockCode::SlotInitFailed:
      o.stage = "host_init";
      o.code = "host_init_failed";
      break;
    case BlockCode::NoCard:
      o.stage = "card_init";
      o.code = "no_card";
      break;
    case BlockCode::SectorTimeout:
      o.stage = "sector_probe";
      o.code = "mount_timeout";
      break;
    case BlockCode::SectorIoError:
    case BlockCode::Oom:
      o.stage = "sector_probe";
      o.code = "io_failure";
      break;
    default:
      o.stage = "sector_probe";
      o.code = "mount_timeout";
      break;
  }
  return o;
}

// Convenience: BlockResult → report strings (same seam as production).
inline ProductionMapOut mapBlockResultToReport(const BlockResult& b) {
  return mapSdmmcFailToReport(static_cast<int>(b.code), static_cast<int>(b.stage));
}

}  // namespace M4SdStatus
