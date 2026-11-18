#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "util_sharedmemory.h"

// Shared memory through which an asynchronous GetRenderTargetData is answered (client exports
// RemixBridge_ReadRenderTargetAsync / RemixBridge_PollReadback). Modelled on QuerySharedBlock: the server
// writes a slot's rows and fields, then publishes answeredSeq with release; the client reads them after an
// acquire load sees its sequence. All fields are 32-bit, so the layout is the same in both processes.

namespace bridge_util {

  struct ReadbackSlot {
    static constexpr uint32_t kMaxDataSize = 256 * 1024;  // one 256x256 A8R8G8B8 tile; larger reads stay synchronous

    std::atomic<uint32_t> answeredSeq;
    int32_t hresult;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t rowSize;
    uint32_t rows;
    uint32_t reserved;
    uint8_t data[kMaxDataSize];
  };
  static_assert(sizeof(ReadbackSlot) == 32 + ReadbackSlot::kMaxDataSize, "ReadbackSlot must have the same layout in both processes");

  struct ReadbackSharedBlock {
    static constexpr uint32_t kMaxSlots = 4;  // 1 MB of the 32-bit client's address space, mapped on first use only

    ReadbackSlot slots[kMaxSlots];
    // The last ticket handed out. It lives here, not in the client: a rewind test host (patch 0015) puts the client's
    // memory back but not this block, so a ticket is never handed out twice and a late answer cannot match a new request.
    std::atomic<uint32_t> lastSeq;
  };

  // Maps the block on first use; nullptr when the mapping failed (callers then use the synchronous path).
  inline ReadbackSharedBlock* readbackBlock() {
    static std::once_flag once;
    static std::unique_ptr<SharedMemory> s_memory;
    std::call_once(once, [] {
      s_memory = std::make_unique<SharedMemory>("Readbacks", sizeof(ReadbackSharedBlock));
      if (s_memory->data() == nullptr) {
        Logger::err("[Bridge] The read-back block could not be mapped, so asynchronous read-backs are off.");
      }
    });
    return static_cast<ReadbackSharedBlock*>(s_memory->data());
  }
}
