#!/usr/bin/env python3
"""Owner-aware storage mutex regression for WebSocket upload handoff.

Compiles a host mock of the production take/give protocol: FreeRTOS mutex
ownership (give only from the taking task). START→READY→Back/Home must give
on the UI task. HTTP holding the mutex must make WS START fail busy and Back
return without an unbounded take.
"""
from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
AUX_H = ROOT / "firmware/src/network/M4FileTransferAuxiliaryServer.h"
AUX_CPP = ROOT / "firmware/src/network/M4FileTransferAuxiliaryServer.cpp"
ACTIVITY = ROOT / "firmware/src/activities/network/CrossPointWebServerActivity.cpp"
SERVICE_CPP = ROOT / "firmware/src/network/M4FileTransferService.cpp"

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using TickType_t = uint32_t;
using BaseType_t = int;
using TaskHandle_t = std::thread::id;
using SemaphoreHandle_t = struct OwnerMutex*;

#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))

struct OwnerMutex {
  std::timed_mutex mu;
  std::thread::id owner{};
  bool held = false;
  std::atomic<int> take_ok{0};
  std::atomic<int> take_fail{0};
  std::atomic<int> give_ok{0};
  std::atomic<int> give_wrong_task{0};
};

static thread_local std::thread::id tls_task{};
static OwnerMutex g_mutex;

TaskHandle_t xTaskGetCurrentTaskHandle() { return tls_task; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks) {
  if (!handle) return pdFALSE;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ticks);
  if (ticks == 0) {
    if (!handle->mu.try_lock()) {
      handle->take_fail.fetch_add(1);
      return pdFALSE;
    }
  } else if (!handle->mu.try_lock_until(deadline)) {
    handle->take_fail.fetch_add(1);
    return pdFALSE;
  }
  if (handle->held) {
    handle->mu.unlock();
    handle->take_fail.fetch_add(1);
    throw std::runtime_error("recursive take on owner-aware mutex");
  }
  handle->held = true;
  handle->owner = tls_task;
  handle->take_ok.fetch_add(1);
  return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle) {
  if (!handle) return pdFALSE;
  if (!handle->held || handle->owner != tls_task) {
    handle->give_wrong_task.fetch_add(1);
    throw std::runtime_error("mutex give from non-owner task");
  }
  handle->held = false;
  handle->owner = {};
  handle->mu.unlock();
  handle->give_ok.fetch_add(1);
  return pdTRUE;
}

class Aux {
 public:
  explicit Aux(SemaphoreHandle_t storageMutex) : storageMutex_(storageMutex) {}

  static constexpr TickType_t kStorageTakeTicks = pdMS_TO_TICKS(50);

  bool acquireStorage() {
    if (storageLocked_) return true;
    if (!storageMutex_) return false;
    if (xSemaphoreTake(storageMutex_, kStorageTakeTicks) != pdTRUE) return false;
    storageLocked_ = true;
    storageOwner_ = xTaskGetCurrentTaskHandle();
    return true;
  }

  void releaseStorage() {
    if (!storageLocked_) return;
    if (storageOwner_ != xTaskGetCurrentTaskHandle()) return;
    storageLocked_ = false;
    storageOwner_ = {};
    xSemaphoreGive(storageMutex_);
  }

  void abortUpload(bool) {
    wsUploadInProgress_ = false;
    releaseStorage();
  }

  void abortOwnedUpload() {
    if (wsUploadInProgress_ || storageLocked_) abortUpload(true);
  }

  void stop() {
    if ((wsUploadInProgress_ || storageLocked_) &&
        storageOwner_ == xTaskGetCurrentTaskHandle()) {
      abortUpload(true);
    } else {
      wsUploadInProgress_ = false;
    }
  }

  bool startUpload() {
    if (wsUploadInProgress_ || storageLocked_) abortUpload(true);
    if (!acquireStorage()) return false;
    wsUploadInProgress_ = true;
    return true;
  }

  bool storageLocked() const { return storageLocked_; }

 private:
  SemaphoreHandle_t storageMutex_ = nullptr;
  TaskHandle_t storageOwner_{};
  bool storageLocked_ = false;
  bool wsUploadInProgress_ = false;
};

int main() {
  tls_task = std::this_thread::get_id();
  Aux aux(&g_mutex);

  // START → READY → Back/Home: take and give on the UI task.
  {
    std::thread ui([&]() {
      tls_task = std::this_thread::get_id();
      assert(aux.startUpload());
      aux.abortOwnedUpload();
      assert(!aux.storageLocked());
      aux.stop();
    });
    ui.join();
    assert(g_mutex.take_ok.load() == 1);
    assert(g_mutex.give_ok.load() == 1);
    assert(g_mutex.give_wrong_task.load() == 0);
    assert(!g_mutex.held);
  }

  g_mutex.take_ok = 0;
  g_mutex.give_ok = 0;

  // Cleanup on another task must not give the UI-owned mutex.
  {
    std::thread ui([&]() {
      tls_task = std::this_thread::get_id();
      assert(aux.startUpload());
      std::thread cleanup([&]() {
        tls_task = std::this_thread::get_id();
        aux.stop();
      });
      cleanup.join();
      assert(aux.storageLocked());
      aux.abortOwnedUpload();
    });
    ui.join();
    assert(g_mutex.give_wrong_task.load() == 0);
    assert(!g_mutex.held);
  }

  g_mutex.take_ok = 0;
  g_mutex.take_fail = 0;
  g_mutex.give_ok = 0;

  // HTTP holds the mutex; WS START must fail busy; Back returns promptly.
  {
    std::atomic<bool> http_holding{false};
    std::atomic<bool> http_done{false};
    std::thread http([&]() {
      tls_task = std::this_thread::get_id();
      assert(xSemaphoreTake(&g_mutex, pdMS_TO_TICKS(10)) == pdTRUE);
      http_holding = true;
      while (!http_done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      assert(xSemaphoreGive(&g_mutex) == pdTRUE);
    });
    while (!http_holding.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::thread ui([&]() {
      tls_task = std::this_thread::get_id();
      const auto t0 = std::chrono::steady_clock::now();
      Aux ws(&g_mutex);
      assert(!ws.startUpload());
      ws.abortOwnedUpload();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
      assert(ms < 500);
      ws.stop();
    });
    ui.join();
    http_done = true;
    http.join();
    assert(g_mutex.take_fail.load() >= 1);
    assert(g_mutex.give_wrong_task.load() == 0);
    assert(!g_mutex.held);
  }

  return 0;
}
'''


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"missing {signature}")
    brace = source.find("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : i + 1]
    raise SystemExit(f"unterminated {signature}")


def source_checks() -> None:
    aux_h = AUX_H.read_text(encoding="utf-8")
    aux_cpp = AUX_CPP.read_text(encoding="utf-8")
    activity = ACTIVITY.read_text(encoding="utf-8")
    service = SERVICE_CPP.read_text(encoding="utf-8")

    assert "abortOwnedUpload" in aux_h
    assert "kStorageTakeTicks" in aux_h
    assert "portMAX_DELAY" not in function_body(aux_cpp, "bool M4FileTransferAuxiliaryServer::acquireStorage()")
    assert "kStorageTakeTicks" in function_body(aux_cpp, "bool M4FileTransferAuxiliaryServer::acquireStorage()")
    assert "storageOwner_ != xTaskGetCurrentTaskHandle()" in function_body(
        aux_cpp, "void M4FileTransferAuxiliaryServer::releaseStorage()"
    )

    stop = function_body(aux_cpp, "void M4FileTransferAuxiliaryServer::stop()")
    assert "abortOwnedUpload" not in stop or "storageOwner_ == xTaskGetCurrentTaskHandle()" in stop
    assert "storageOwner_ == xTaskGetCurrentTaskHandle()" in stop

    exit_body = function_body(activity, "void CrossPointWebServerActivity::onExit()")
    abort_pos = exit_body.find("abortOwnedWsUpload()")
    notify_pos = exit_body.find("xTaskNotifyGive(")
    assert abort_pos >= 0 and notify_pos >= 0 and abort_pos < notify_pos
    assert "fileTransferService.stop(" not in exit_body
    assert "fileTransferService().stop(" not in exit_body

    stop_web = function_body(service, "void M4FileTransferService::stopWebServer()")
    aux_stop = stop_web.find("auxiliaryServer->stop()")
    httpd = stop_web.find("httpd_stop")
    routes = stop_web.find("httpRoutes.reset()")
    mutex_del = stop_web.find("vSemaphoreDelete(runtime.storageMutex)")
    assert 0 <= aux_stop < httpd < routes < mutex_del


def compile_and_run() -> None:
    with tempfile.TemporaryDirectory(prefix="m4-ws-owner-") as tmp:
        cpp = Path(tmp) / "test.cpp"
        exe = Path(tmp) / "test"
        cpp.write_text(HARNESS, encoding="utf-8")
        compiled = subprocess.run(
            [
                "c++",
                "-std=c++17",
                "-O1",
                "-g",
                "-pthread",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(cpp),
                "-o",
                str(exe),
            ],
            capture_output=True,
            text=True,
        )
        if compiled.returncode != 0:
            sys.stderr.write(compiled.stderr)
            sys.stderr.write(compiled.stdout)
            raise SystemExit(f"compile failed ({compiled.returncode})")
        t0 = time.monotonic()
        ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
        elapsed = time.monotonic() - t0
        sys.stdout.write(ran.stdout)
        if ran.returncode != 0:
            sys.stderr.write(ran.stderr)
            raise SystemExit(f"host owner test failed ({ran.returncode})")
        if elapsed >= 2.0:
            raise SystemExit(f"host owner test took too long: {elapsed:.3f}s")
        print(f"m4 ws storage owner host: PASS ({elapsed:.3f}s)")


def main() -> None:
    source_checks()
    print("m4 ws storage owner source: PASS")
    compile_and_run()


if __name__ == "__main__":
    main()
