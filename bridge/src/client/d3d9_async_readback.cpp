/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "pch.h"

#include <cstring>
#include <mutex>

#include "d3d9_device_base.h"
#include "d3d9_surface.h"
#include "util_devicecommand.h"
#include "util_readbacks.h"
#include "util_texture_and_volume.h"

// Asynchronous GetRenderTargetData for callers that can wait a frame or two (client.asyncReadback).
// The game's own GetRenderTargetData calls stay synchronous. Only callers of these exports go asynchronous.
//
// RemixBridge_ReadRenderTargetAsync(rt, dst, &ticket) queues the copy and returns at once:
//   S_OK with a ticket.
//   E_NOTIMPL when client.asyncReadback is off or the shared block is missing.
//   D3DERR_WASSTILLDRAWING when all slots are in flight.
//   D3DERR_INVALIDCALL for a bad surface or a read larger than one slot.
// RemixBridge_PollReadback(ticket) returns S_FALSE while pending.
//   It returns S_OK once the rows are in dst's client copy, as after a synchronous GetRenderTargetData.
//   It returns the server's error, D3DERR_INVALIDCALL for an unknown ticket, or D3DERR_DEVICELOST without a server.
// RemixBridge_CancelReadback(ticket) frees the slot of a ticket the caller gives up on.
// A finished, failed or cancelled ticket is unknown afterwards.

namespace {
  using bridge_util::ReadbackSharedBlock;
  using bridge_util::ReadbackSlot;

  struct Pending {
    Direct3DSurface9_LSS* pDst = nullptr;  // holds a reference, so the server's surface outlives the request
    uint32_t seq = 0;
  };

  std::mutex s_mutex;
  Pending s_pending[ReadbackSharedBlock::kMaxSlots];

  bool asyncReadbackEnabled() {
    static const bool enabled = bridge_util::Config::getOption<bool>("client.asyncReadback", false);
    return enabled;
  }

  Pending* findTicket(uint32_t ticket) {
    for (auto& pending : s_pending) {
      if (pending.seq != 0 && pending.seq == ticket) {
        return &pending;
      }
    }
    return nullptr;
  }

  void freePending(Pending& pending) {
    pending.pDst->Release();
    pending = Pending {};
  }

  HRESULT copySlot(const ReadbackSlot& slot, Direct3DSurface9_LSS* pDst) {
    const D3DSURFACE_DESC desc = pDst->getDesc();
    if (slot.width != desc.Width || slot.height != desc.Height || slot.format != (uint32_t) desc.Format ||
        size_t(slot.rowSize) * slot.rows > ReadbackSlot::kMaxDataSize) {
      return D3DERR_INVALIDCALL;
    }

    // The same lock and row copy as the synchronous path (copyServerSurfaceRawData).
    D3DLOCKED_RECT lockedRect;
    HRESULT hresult = pDst->LockRect(&lockedRect, nullptr, D3DLOCK_DISCARD);
    if (hresult != S_OK) {
      return hresult;
    }
    for (uint32_t y = 0; y < slot.rows; ++y) {
      memcpy((uint8_t*) lockedRect.pBits + size_t(y) * lockedRect.Pitch, slot.data + size_t(y) * slot.rowSize, slot.rowSize);
    }
    return pDst->UnlockRect();
  }
}

// Exported by name through d3d9_lss.def.
extern "C" {

  HRESULT __stdcall RemixBridge_ReadRenderTargetAsync(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface, uint32_t* pTicket) {
    if (pRenderTarget == nullptr || pDestSurface == nullptr || pTicket == nullptr) {
      return D3DERR_INVALIDCALL;
    }
    if (!asyncReadbackEnabled() || bridge_util::readbackBlock() == nullptr) {
      return E_NOTIMPL;
    }
    if (!gbBridgeRunning) {
      return D3DERR_DEVICELOST;
    }
    const auto pLssSource = bridge_cast<Direct3DSurface9_LSS*>(pRenderTarget);
    const auto pLssDest = bridge_cast<Direct3DSurface9_LSS*>(pDestSurface);
    if (pLssSource == nullptr || pLssDest == nullptr) {
      return D3DERR_INVALIDCALL;
    }
    const D3DSURFACE_DESC desc = pLssDest->getDesc();
    if (desc.Pool != D3DPOOL_SYSTEMMEM ||
        bridge_util::calcTotalSizeOfRect(desc.Width, desc.Height, desc.Format) > ReadbackSlot::kMaxDataSize) {
      return D3DERR_INVALIDCALL;
    }

    IDirect3DDevice9* pDevice = nullptr;
    if (FAILED(pLssDest->GetDevice(&pDevice)) || pDevice == nullptr) {
      return D3DERR_INVALIDCALL;
    }
    const size_t deviceId = static_cast<BaseDirect3DDevice9Ex_LSS*>(pDevice)->getId();
    pDevice->Release();

    std::lock_guard<std::mutex> lock(s_mutex);
    uint32_t slotIndex = ReadbackSharedBlock::kMaxSlots;
    for (uint32_t i = 0; i < ReadbackSharedBlock::kMaxSlots; ++i) {
      if (s_pending[i].seq == 0) {
        slotIndex = i;
        break;
      }
    }
    if (slotIndex == ReadbackSharedBlock::kMaxSlots) {
      return D3DERR_WASSTILLDRAWING;
    }

    // Sequence numbers are never 0, because 0 marks a free slot. They come from the shared block (see lastSeq).
    std::atomic<uint32_t>& lastSeq = bridge_util::readbackBlock()->lastSeq;
    uint32_t seq = lastSeq.fetch_add(1) + 1;
    if (seq == 0) {
      seq = lastSeq.fetch_add(1) + 1;
    }
    pLssDest->AddRef();
    s_pending[slotIndex] = Pending { pLssDest, seq };
    {
      ClientMessage c(Commands::IDirect3DDevice9Ex_GetRenderTargetData, deviceId, Commands::FlagBits::AsyncResult);
      c.send_data(pLssSource->getId());
      c.send_data(pLssDest->getId());
      c.send_data(slotIndex);
      c.send_data(seq);
    }
    *pTicket = seq;
    return S_OK;
  }

  HRESULT __stdcall RemixBridge_PollReadback(uint32_t ticket) {
    std::lock_guard<std::mutex> lock(s_mutex);
    Pending* const pPending = findTicket(ticket);
    if (pPending == nullptr) {
      return D3DERR_INVALIDCALL;
    }
    const ReadbackSlot& slot = bridge_util::readbackBlock()->slots[pPending - s_pending];
    if (slot.answeredSeq.load(std::memory_order_acquire) != ticket) {
      if (gbBridgeRunning) {
        return S_FALSE;
      }
      freePending(*pPending);
      return D3DERR_DEVICELOST;
    }
    HRESULT hresult = (HRESULT) slot.hresult;
    if (hresult == S_OK) {
      hresult = copySlot(slot, pPending->pDst);
    }
    freePending(*pPending);
    return hresult;
  }

  HRESULT __stdcall RemixBridge_CancelReadback(uint32_t ticket) {
    std::lock_guard<std::mutex> lock(s_mutex);
    Pending* const pPending = findTicket(ticket);
    if (pPending == nullptr) {
      return D3DERR_INVALIDCALL;
    }
    freePending(*pPending);
    return S_OK;
  }
}
