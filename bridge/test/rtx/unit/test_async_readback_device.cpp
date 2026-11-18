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

// Reads a ColorFill pattern back through the synchronous GetRenderTargetData and through the
// RemixBridge_ReadRenderTargetAsync / RemixBridge_PollReadback exports, and compares the bytes.
// REMIX_ASYNC_READBACK_TEST_CASE=off runs with client.asyncReadback = False and expects E_NOTIMPL.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <wrl/client.h>

#include "test_remix_api_bridge_app.h"

using Microsoft::WRL::ComPtr;
using namespace bridge_app_test;

namespace {
  using PFN_ReadAsync = HRESULT (__stdcall*)(IDirect3DSurface9*, IDirect3DSurface9*, uint32_t*);
  using PFN_Ticket = HRESULT (__stdcall*)(uint32_t);

  constexpr UINT kWindowSize = 64;

  PFN_ReadAsync g_readAsync = nullptr;
  PFN_Ticket g_poll = nullptr;
  PFN_Ticket g_cancel = nullptr;

  void check(HRESULT result, const char* pCall) {
    if (FAILED(result)) {
      std::printf("FAILED: %s returned 0x%08lX.\n", pCall, static_cast<unsigned long>(result));
      throw std::runtime_error(pCall);
    }
  }

  void expect(HRESULT result, HRESULT expected, const char* pCall) {
    if (result != expected) {
      std::printf("FAILED: %s returned 0x%08lX, expected 0x%08lX.\n", pCall,
                  static_cast<unsigned long>(result), static_cast<unsigned long>(expected));
      throw std::runtime_error(pCall);
    }
  }

  HWND createWindow() {
    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"BridgeAsyncReadbackTest";
    RegisterClassW(&windowClass);
    return CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                           0, 0, kWindowSize, kWindowSize, nullptr, nullptr, windowClass.hInstance, nullptr);
  }

  struct Target {
    ComPtr<IDirect3DSurface9> pRenderTarget;
    ComPtr<IDirect3DSurface9> pSync;
    ComPtr<IDirect3DSurface9> pAsync;
    UINT size = 0;
  };

  Target createTarget(IDirect3DDevice9* pDevice, UINT size) {
    Target target;
    target.size = size;
    check(pDevice->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                      &target.pRenderTarget, nullptr), "CreateRenderTarget");
    check(pDevice->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &target.pSync, nullptr),
          "CreateOffscreenPlainSurface (sync)");
    check(pDevice->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &target.pAsync, nullptr),
          "CreateOffscreenPlainSurface (async)");
    return target;
  }

  // Four quadrants of different colours, varied by seed, so a stale or shifted read-back differs.
  void fill(IDirect3DDevice9* pDevice, const Target& target, uint32_t seed) {
    const LONG half = LONG(target.size / 2);
    const RECT quads[4] = { { 0, 0, half, half }, { half, 0, LONG(target.size), half },
                            { 0, half, half, LONG(target.size) }, { half, half, LONG(target.size), LONG(target.size) } };
    for (uint32_t i = 0; i < 4; ++i) {
      check(pDevice->ColorFill(target.pRenderTarget.Get(), &quads[i], 0xff000000 | ((seed * 0x2f + i * 0x51) * 0x010307)), "ColorFill");
    }
  }

  std::vector<uint8_t> readBytes(IDirect3DSurface9* pSurface, UINT size) {
    D3DLOCKED_RECT lockedRect;
    check(pSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY), "LockRect");
    std::vector<uint8_t> bytes(size_t(size) * size * 4);
    for (UINT y = 0; y < size; ++y) {
      memcpy(bytes.data() + size_t(y) * size * 4, (const uint8_t*) lockedRect.pBits + size_t(y) * lockedRect.Pitch, size_t(size) * 4);
    }
    check(pSurface->UnlockRect(), "UnlockRect");
    return bytes;
  }

  // Presents between polls, as a game does, so the server reaches its poll points.
  unsigned waitForTicket(IDirect3DDevice9* pDevice, uint32_t ticket) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    for (unsigned polls = 1;; ++polls) {
      const HRESULT result = g_poll(ticket);
      if (result == S_OK) {
        return polls;
      }
      if (result != S_FALSE) {
        expect(result, S_OK, "RemixBridge_PollReadback");
      }
      if (GetTickCount64() >= deadline) {
        throw std::runtime_error("The read-back did not finish within ten seconds");
      }
      check(pDevice->Present(nullptr, nullptr, nullptr, nullptr), "Present");
    }
  }

  void testMatch(IDirect3DDevice9* pDevice, const Target& target, uint32_t seed) {
    fill(pDevice, target, seed);
    check(pDevice->GetRenderTargetData(target.pRenderTarget.Get(), target.pSync.Get()), "GetRenderTargetData");
    uint32_t ticket = 0;
    check(g_readAsync(target.pRenderTarget.Get(), target.pAsync.Get(), &ticket), "RemixBridge_ReadRenderTargetAsync");
    const unsigned polls = waitForTicket(pDevice, ticket);
    if (readBytes(target.pSync.Get(), target.size) != readBytes(target.pAsync.Get(), target.size)) {
      throw std::runtime_error("The asynchronous read-back differs from the synchronous one");
    }
    expect(g_poll(ticket), D3DERR_INVALIDCALL, "RemixBridge_PollReadback of a finished ticket");
    std::printf("  %ux%u read-back matched after %u polls.\n", target.size, target.size, polls);
  }

  // Fills all slots, checks the refusal and a cancel, then finishes the rest in order.
  void testSlots(IDirect3DDevice9* pDevice, const Target& target) {
    logLine("Testing four read-backs in flight, a fifth refused and one cancelled...");
    std::vector<Target> targets;
    uint32_t tickets[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      targets.push_back(createTarget(pDevice, 16));
      fill(pDevice, targets[i], 10 + i);
      check(g_readAsync(targets[i].pRenderTarget.Get(), targets[i].pAsync.Get(), &tickets[i]), "RemixBridge_ReadRenderTargetAsync (slot)");
    }
    uint32_t extra = 0;
    expect(g_readAsync(target.pRenderTarget.Get(), target.pAsync.Get(), &extra), D3DERR_WASSTILLDRAWING, "Fifth read-back");
    check(g_cancel(tickets[0]), "RemixBridge_CancelReadback");
    expect(g_poll(tickets[0]), D3DERR_INVALIDCALL, "RemixBridge_PollReadback of a cancelled ticket");

    // The freed slot takes a new request while the cancelled one may still be answered on the server.
    testMatch(pDevice, targets[0], 20);
    for (uint32_t i = 1; i < 4; ++i) {
      waitForTicket(pDevice, tickets[i]);
      check(pDevice->GetRenderTargetData(targets[i].pRenderTarget.Get(), targets[i].pSync.Get()), "GetRenderTargetData (slot)");
      if (readBytes(targets[i].pSync.Get(), 16) != readBytes(targets[i].pAsync.Get(), 16)) {
        throw std::runtime_error("A queued read-back differs from the synchronous one");
      }
    }
  }
}

// Names the faulting module and offset, so a crash in a run without a debugger can be traced.
LONG WINAPI logCrash(EXCEPTION_POINTERS* pInfo) {
  const auto address = reinterpret_cast<uintptr_t>(pInfo->ExceptionRecord->ExceptionAddress);
  HMODULE hModule = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &hModule)) {
    GetModuleFileNameA(hModule, name, MAX_PATH);
  }
  std::printf("CRASH: exception 0x%08lX at %s+0x%lX\n", static_cast<unsigned long>(pInfo->ExceptionRecord->ExceptionCode),
              name, static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(hModule)));
  return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SetUnhandledExceptionFilter(logCrash);
  char mode[8] = {};
  GetEnvironmentVariableA("REMIX_ASYNC_READBACK_TEST_CASE", mode, sizeof(mode));
  const bool enabled = strcmp(mode, "off") != 0;
  const HWND hWnd = createWindow();
  if (hWnd == nullptr) {
    logLine("FAILED: Could not create a test window.");
    return 1;
  }
  bool passed = false;
  try {
    // The client reads its options from .trex/bridge.conf, as in test_query_event_device.
    std::ofstream config(".trex/bridge.conf", std::ios::app);
    config << "client.asyncReadback = " << (enabled ? "True" : "False") << "\n"
           << "sendCreateFunctionServerResponses = True\n";
    config.close();
    const HMODULE hClient = loadBridgeClient();
    if (hClient == nullptr) {
      throw std::runtime_error("loadBridgeClient");
    }
    g_readAsync = reinterpret_cast<PFN_ReadAsync>(GetProcAddress(hClient, "RemixBridge_ReadRenderTargetAsync"));
    g_poll = reinterpret_cast<PFN_Ticket>(GetProcAddress(hClient, "RemixBridge_PollReadback"));
    g_cancel = reinterpret_cast<PFN_Ticket>(GetProcAddress(hClient, "RemixBridge_CancelReadback"));
    if (g_readAsync == nullptr || g_poll == nullptr || g_cancel == nullptr) {
      throw std::runtime_error("The client does not export the asynchronous read-back functions");
    }
    const auto pfnCreate = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(hClient, "Direct3DCreate9"));
    ComPtr<IDirect3D9> pD3D;
    pD3D.Attach(pfnCreate != nullptr ? pfnCreate(D3D_SDK_VERSION) : nullptr);
    if (pD3D == nullptr) {
      throw std::runtime_error("Direct3DCreate9");
    }
    D3DPRESENT_PARAMETERS present = {};
    present.Windowed = TRUE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_UNKNOWN;
    present.BackBufferWidth = kWindowSize;
    present.BackBufferHeight = kWindowSize;
    present.hDeviceWindow = hWnd;
    ComPtr<IDirect3DDevice9> pDevice;
    check(pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                              D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                              &present, &pDevice), "CreateDevice");

    const Target pick = createTarget(pDevice.Get(), 16);
    if (!enabled) {
      logLine("Testing that the exports refuse while client.asyncReadback is off...");
      uint32_t ticket = 0;
      expect(g_readAsync(pick.pRenderTarget.Get(), pick.pAsync.Get(), &ticket), E_NOTIMPL, "RemixBridge_ReadRenderTargetAsync (off)");
    } else {
      logLine("Testing asynchronous read-backs against the synchronous path...");
      const Target tile = createTarget(pDevice.Get(), 256);
      testMatch(pDevice.Get(), pick, 1);
      testMatch(pDevice.Get(), tile, 2);
      testMatch(pDevice.Get(), pick, 3);
      testSlots(pDevice.Get(), pick);

      logLine("Testing that a read larger than one slot is refused...");
      const Target big = createTarget(pDevice.Get(), 512);
      uint32_t ticket = 0;
      expect(g_readAsync(big.pRenderTarget.Get(), big.pAsync.Get(), &ticket), D3DERR_INVALIDCALL, "RemixBridge_ReadRenderTargetAsync (512x512)");
    }
    passed = true;
  } catch (const std::exception& error) {
    std::printf("FAILED: %s.\n", error.what());
  }
  DestroyWindow(hWnd);
  logLine(passed ? "PASSED" : "FAILED");
  return passed ? 0 : 1;
}
