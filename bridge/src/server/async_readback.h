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
#pragma once

#include <cstring>
#include <deque>

#include <d3d9.h>

#include "util_readbacks.h"
#include "util_texture_and_volume.h"

// Rewind mode (patch 0015) is optional, so this patch builds with and without it.
#if __has_include("rewind_mode.h")
#include "rewind_mode.h"
#define ASYNC_READBACK_HAS_REWIND 1
#endif

// Server side of the asynchronous GetRenderTargetData (client/d3d9_async_readback.cpp).
// The copy is queued on the device with an event query behind it, and the command loop moves on.
// Requests are answered in order, once their event signals, so the read-only lock no longer waits for the GPU.
// The rows then go into the request's shared slot, and the slot's sequence is published last.
namespace async_readback {
  struct Pending {
    IDirect3DSurface9* pDst;  // referenced until answered
    IDirect3DQuery9* pQuery;  // nullptr when the request already failed
    HRESULT hresult;
    uint32_t slot;
    uint32_t seq;
  };

  inline std::deque<Pending> g_pending;
  inline uint32_t g_commandsSincePoll = 0;

  inline HRESULT copyRows(IDirect3DSurface9* pDst, bridge_util::ReadbackSlot& slot) {
    D3DSURFACE_DESC desc;
    HRESULT hresult = pDst->GetDesc(&desc);
    if (FAILED(hresult)) {
      return hresult;
    }
    const uint32_t rowSize = bridge_util::calcRowSize(desc.Width, desc.Format);
    const uint32_t rows = bridge_util::calcStride(desc.Height, desc.Format);
    if (size_t(rowSize) * rows > bridge_util::ReadbackSlot::kMaxDataSize) {
      return D3DERR_INVALIDCALL;
    }
    D3DLOCKED_RECT lockedRect;
    hresult = pDst->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
    if (FAILED(hresult)) {
      return hresult;
    }
    for (uint32_t y = 0; y < rows; ++y) {
      memcpy(slot.data + size_t(y) * rowSize, (const uint8_t*) lockedRect.pBits + size_t(y) * lockedRect.Pitch, rowSize);
    }
    pDst->UnlockRect();
    slot.width = desc.Width;
    slot.height = desc.Height;
    slot.format = (uint32_t) desc.Format;
    slot.rowSize = rowSize;
    slot.rows = rows;
    return S_OK;
  }

  inline void answer(Pending& pending, HRESULT hresult) {
    bridge_util::ReadbackSlot& slot = bridge_util::readbackBlock()->slots[pending.slot];
    if (hresult == S_OK) {
      hresult = copyRows(pending.pDst, slot);
    }
    slot.hresult = (int32_t) hresult;
    slot.answeredSeq.store(pending.seq, std::memory_order_release);
    if (pending.pQuery != nullptr) {
      pending.pQuery->Release();
    }
    if (pending.pDst != nullptr) {
      pending.pDst->Release();
    }
  }

  // Answers the finished requests at the front of the queue. Event queries signal in submission order.
  inline void poll() {
    g_commandsSincePoll = 0;
    while (!g_pending.empty()) {
      Pending& pending = g_pending.front();
      HRESULT hresult = pending.hresult;
      if (pending.pQuery != nullptr) {
        const HRESULT status = pending.pQuery->GetData(nullptr, 0, 0);
        if (status == S_FALSE) {
          return;
        }
        hresult = status;
      }
      answer(pending, hresult);
      g_pending.pop_front();
    }
  }

  // Polls every 64 commands, so a request is answered within the frame that finished its copy.
  inline void onCommand() {
    if (!g_pending.empty() && ++g_commandsSincePoll >= 64) {
      poll();
    }
  }

  // A device reset or destroy fails the open requests. The caller then reads synchronously.
  inline void dropAll() {
    for (Pending& pending : g_pending) {
      answer(pending, D3DERR_DEVICELOST);
    }
    g_pending.clear();
  }

  inline void request(IDirect3DDevice9* pDevice, IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDst, uint32_t slot, uint32_t seq) {
    if (bridge_util::readbackBlock() == nullptr || slot >= bridge_util::ReadbackSharedBlock::kMaxSlots) {
      return;
    }
    Pending pending { nullptr, nullptr, S_OK, slot, seq };
#ifdef ASYNC_READBACK_HAS_REWIND
    // The copy writes dst as the synchronous GetRenderTargetData does, so a held snapshot keeps its old contents.
    rewind_mode::keepSurface(pDst);
#endif
    HRESULT hresult = pRenderTarget != nullptr && pDst != nullptr
      ? pDevice->GetRenderTargetData(pRenderTarget, pDst)
      : D3DERR_INVALIDCALL;
    if (SUCCEEDED(hresult)) {
      hresult = pDevice->CreateQuery(D3DQUERYTYPE_EVENT, &pending.pQuery);
    }
    if (SUCCEEDED(hresult)) {
      hresult = pending.pQuery->Issue(D3DISSUE_END);
      // The flush submits the copy now, so it runs while the server replays the rest of the frame.
      pending.pQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    }
    if (FAILED(hresult)) {
      if (pending.pQuery != nullptr) {
        pending.pQuery->Release();
        pending.pQuery = nullptr;
      }
      pending.hresult = hresult;
    } else {
      pDst->AddRef();
      pending.pDst = pDst;
    }
    // A failure is queued as well, so a reused slot is answered in request order.
    g_pending.push_back(pending);
  }
}
