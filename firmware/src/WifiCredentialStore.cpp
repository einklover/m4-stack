#include "WifiCredentialStore.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <algorithm>
#include <cstdint>
#include <utility>



// Initialize the static instance
WifiCredentialStore WifiCredentialStore::instance;

namespace {
// File format version
constexpr uint8_t WIFI_FILE_VERSION = 1;

// WiFi credentials file path
constexpr char WIFI_FILE[] = "/.crosspoint/wifi.bin";

// Obfuscation key - "CrossPoint" in ASCII
// This is NOT cryptographic security, just prevents casual file reading
constexpr uint8_t OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t KEY_LENGTH = sizeof(OBFUSCATION_KEY);
}  // namespace

void WifiCredentialStore::obfuscate(std::string& data) const {
  Serial.printf("[%lu] [WCS] Obfuscating/deobfuscating %zu bytes\n", millis(), data.size());
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= OBFUSCATION_KEY[i % KEY_LENGTH];
  }
}

namespace {

bool writeExact(FsFile& file, const uint8_t* data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const int written = file.write(data + offset, size - offset);
    if (written <= 0) return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

template <typename T>
bool writePodExact(FsFile& file, const T& value) {
  return writeExact(file, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
}

bool writeStringExact(FsFile& file, const std::string& value) {
  const uint32_t size = static_cast<uint32_t>(value.size());
  return writePodExact(file, size) && writeExact(file, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

bool readExact(FsFile& file, uint8_t* data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const int read = file.read(data + offset, size - offset);
    if (read <= 0) return false;
    offset += static_cast<size_t>(read);
  }
  return true;
}

template <typename T>
bool readPodExact(FsFile& file, T& value) {
  return readExact(file, reinterpret_cast<uint8_t*>(&value), sizeof(value));
}

bool readStringExact(FsFile& file, std::string& value) {
  uint32_t size = 0;
  if (!readPodExact(file, size) || size > 4096) return false;
  value.resize(size);
  return readExact(file, reinterpret_cast<uint8_t*>(value.data()), size);
}

}  // namespace

bool WifiCredentialStore::saveCredentials(const std::vector<WifiCredential>& next) const {
  // SdFat mkdir() returns false when the directory already exists. Other stores
  // ignore that; treating it as fatal made every Wi-Fi save fail after first boot
  // (recents/settings already created /.crosspoint) → "Connected, but could not
  // save Wi-Fi" and later WeRead no_saved_wifi.
  if (!SdMan.exists("/.crosspoint") && !SdMan.mkdir("/.crosspoint")) {
    Serial.printf("[%lu] [WCS] mkdir /.crosspoint failed\n", millis());
    return false;
  }

  const char* const tempPath = "/.crosspoint/wifi.bin.part";
  const char* const backupPath = "/.crosspoint/wifi.bin.bak";
  if (SdMan.exists(tempPath)) SdMan.remove(tempPath);

  FsFile file;
  if (!SdMan.openFileForWrite("WCS", tempPath, file)) {
    return false;
  }

  const uint8_t count = static_cast<uint8_t>(next.size());
  bool ok = writePodExact(file, WIFI_FILE_VERSION) && writePodExact(file, count);

  for (const auto& cred : next) {
    // Write SSID (plaintext - not sensitive)
    ok = ok && writeStringExact(file, cred.ssid);
    Serial.printf("[%lu] [WCS] Saving SSID: %s, password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());

    // Write password (obfuscated)
    std::string obfuscatedPwd = cred.password;
    obfuscate(obfuscatedPwd);
    ok = ok && writeStringExact(file, obfuscatedPwd);
  }

  ok = ok && file.sync() && file.close();
  if (!ok) {
    file.close();
    SdMan.remove(tempPath);
    return false;
  }

  FsFile probe;
  if (!SdMan.openFileForRead("WCS", tempPath, probe)) {
    SdMan.remove(tempPath);
    return false;
  }
  const size_t writtenSize = static_cast<size_t>(probe.fileSize());
  probe.close();
  if (writtenSize == 0) {
    SdMan.remove(tempPath);
    return false;
  }

  // Keep the last complete file until the new file has been renamed into
  // place. This also makes a failed persistence operation non-destructive.
  if (SdMan.exists(backupPath)) SdMan.remove(backupPath);
  const bool hadFile = SdMan.exists(WIFI_FILE);
  if (hadFile && !SdMan.rename(WIFI_FILE, backupPath)) {
    SdMan.remove(tempPath);
    return false;
  }
  if (!SdMan.rename(tempPath, WIFI_FILE)) {
    if (hadFile && SdMan.exists(backupPath)) SdMan.rename(backupPath, WIFI_FILE);
    SdMan.remove(tempPath);
    return false;
  }

  FsFile verify;
  if (!SdMan.openFileForRead("WCS", WIFI_FILE, verify) ||
      static_cast<size_t>(verify.fileSize()) != writtenSize) {
    verify.close();
    SdMan.remove(WIFI_FILE);
    if (hadFile && SdMan.exists(backupPath)) SdMan.rename(backupPath, WIFI_FILE);
    return false;
  }
  verify.close();
  if (SdMan.exists(backupPath)) SdMan.remove(backupPath);
  Serial.printf("[%lu] [WCS] Saved %zu WiFi credentials to file\n", millis(), next.size());
  return true;
}

bool WifiCredentialStore::saveToFile() const { return saveCredentials(credentials); }

bool WifiCredentialStore::loadFromFile() {
  FsFile file;
  if (!SdMan.openFileForRead("WCS", WIFI_FILE, file)) {
    return false;
  }

  // Read and verify version
  uint8_t version = 0;
  if (!readPodExact(file, version) || version != WIFI_FILE_VERSION) {
    Serial.printf("[%lu] [WCS] Unknown file version: %u\n", millis(), version);
    file.close();
    return false;
  }

  // Read credential count
  uint8_t count = 0;
  if (!readPodExact(file, count) || count > MAX_NETWORKS) {
    file.close();
    return false;
  }

  std::vector<WifiCredential> loaded;
  loaded.reserve(count);
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;

    // Read SSID
    if (!readStringExact(file, cred.ssid)) {
      file.close();
      return false;
    }

    // Read and deobfuscate password
    if (!readStringExact(file, cred.password)) {
      file.close();
      return false;
    }
    Serial.printf("[%lu] [WCS] Loaded SSID: %s, obfuscated password length: %zu\n", millis(), cred.ssid.c_str(),
                  cred.password.size());
    obfuscate(cred.password);  // XOR is symmetric, so same function deobfuscates
    Serial.printf("[%lu] [WCS] After deobfuscation, password length: %zu\n", millis(), cred.password.size());

    loaded.push_back(std::move(cred));
  }

  file.close();
  credentials.swap(loaded);
  Serial.printf("[%lu] [WCS] Loaded %zu WiFi credentials from file\n", millis(), credentials.size());
  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  std::vector<WifiCredential> next = credentials;
  // Check if this SSID already exists and update it
  const auto cred = std::find_if(next.begin(), next.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != next.end()) {
    cred->password = password;
  } else if (next.size() >= MAX_NETWORKS) {
    Serial.printf("[%lu] [WCS] Cannot add more networks, limit of %zu reached\n", millis(), MAX_NETWORKS);
    return false;
  } else {
    next.push_back({ssid, password});
  }

  if (!saveCredentials(next)) return false;
  credentials.swap(next);
  Serial.printf("[%lu] [WCS] Stored credentials for: %s\n", millis(), ssid.c_str());
  return true;
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  std::vector<WifiCredential> next = credentials;
  const auto cred = std::find_if(next.begin(), next.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  if (cred != next.end()) {
    next.erase(cred);
    if (!saveCredentials(next)) return false;
    credentials.swap(next);
    Serial.printf("[%lu] [WCS] Removed credentials for: %s\n", millis(), ssid.c_str());
    return true;
  }
  return false;  // Not found
}

const WifiCredential* WifiCredentialStore::findCredential(const std::string& ssid) const {
  const auto cred = std::find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });

  if (cred != credentials.end()) {
    return &*cred;
  }

  return nullptr;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const { return findCredential(ssid) != nullptr; }

void WifiCredentialStore::clearAll() {
  std::vector<WifiCredential> next;
  if (saveCredentials(next)) {
    credentials.swap(next);
    Serial.printf("[%lu] [WCS] Cleared all WiFi credentials\n", millis());
  }
}
