// Weread protocol helpers (sign / _e / md5 / shard decode) for M4x host.
// Algorithm ported from papers3-weread / weread.koplugin — no UI.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace weread_crypto {

std::string md5Hex(const uint8_t* data, size_t len);
std::string md5Hex(const std::string& s);

// JS encodeURIComponent-style (A-Z a-z 0-9 - _ . ~)
std::string urlEncode(const std::string& s);

// Weread _e() deterministic encoding for bookId / chapterUid / timestamps.
std::string e(const std::string& value);

// Signature s over sorted query string (initial 0x15051505).
std::string sign(const std::string& query);

// Sort params by key, build query (skip key "s"), and compute sign.
// Returns "k=v&..." including trailing &s=<sign>.
std::string sortedQueryWithSign(const std::vector<std::pair<std::string, std::string>>& params,
                                std::string& outQueryForSign);

// Build JSON body for chapter shard POST (e_0/e_1/e_3 or t_0/t_1).
// unixTimeSec: current unix seconds; rnd0to9999: random in [0,9999].
std::string makeContentParamsJson(const std::string& bookId, const std::string& chapterUid,
                                  const std::string& psvts, bool style, int sc, long unixTimeSec,
                                  long rnd0to9999);

// Decode content shards (e0+e1+e3 or t0+t1). Empty inputs allowed for missing shards.
// Returns UTF-8 plaintext on success; empty string on failure.
std::string decodeContentShards(const std::string& s0, const std::string& s1, const std::string& s2);

// Extract "psvts":"..." from reader HTML.
std::string extractPsvts(const std::string& html);

// Strip common XHTML tags to plain text (best-effort for reading).
std::string stripXhtml(const std::string& xhtml);

}  // namespace weread_crypto
