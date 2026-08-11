#pragma once

// Isolated step runner for M4HttpTransport base debugging.
// Invoked from m4adb `http_probe` so WeRead chapter fetch can be exercised
// one step at a time without launching the full native reader UI.

#include "apps/M4HttpTransport.h"

#include <cstddef>
#include <cstdint>

namespace M4HttpTransportProbe {

// step values (ASCII, case-sensitive):
//   mem              — heap / TLS gate only
//   debug_on         — Serial [M4Http] on + SD log path
//   debug_off
//   session_begin    — host= (default weread.qq.com)
//   session_end
//   tls_get          — GET url= (default https://weread.qq.com/), session=0|1
//   weread_psvts     — bookId= chapterUid=  (cookie from apps_data)
//   weread_e0        — bookId= chapterUid= [psvts=]  (uses last psvts if empty)
//   weread_t0 / weread_t1 / weread_e1 / weread_e3
//   weread_set_cookie — wr_vid / wr_skey / wr_rt (debug only; writes config.json)
//   weread_worker_fetch — full NativeProvider worker path (ensureBook+ensureChapter)
//                        bookId= chapterUid=  [optional title=]
//   shutdown         — sessionEnd + clear last psvts

struct Args {
  const char* step = nullptr;
  const char* host = nullptr;       // session_begin
  const char* url = nullptr;        // tls_get
  bool useSession = true;
  const char* bookId = nullptr;
  const char* chapterUid = nullptr;
  const char* title = nullptr;      // weread_worker_fetch optional
  const char* psvts = nullptr;      // optional override for shard steps
  const char* appId = nullptr;      // default com.weread.client
  const char* wrVid = nullptr;      // weread_set_cookie
  const char* wrSkey = nullptr;
  const char* wrRt = nullptr;
  uint32_t timeoutMs = 30000;
};

struct Result {
  bool ok = false;
  char step[24] = {};
  char error[48] = {};
  int status = 0;
  size_t bytes = 0;
  M4HttpTransport::MemSnap before{};
  M4HttpTransport::MemSnap after{};
  bool sessionOpen = false;
  char detail[96] = {};   // psvts preview / body prefix / note
  char psvts[80] = {};    // last extracted psvts (truncated for USB reply)
};

// Runs one step synchronously (main loop). Returns false only on unknown step;
// transport/network failures set result.ok=false and fill error.
bool run(const Args& args, Result& out);

// Last successful psvts from weread_psvts (full, not truncated).
const char* lastPsvts();

}  // namespace M4HttpTransportProbe
