#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "WifiCredentialStore.h"
#include "apps/M4xWifiConnect.h"

#include "wifi_store_shims/SDCardManager.h"

namespace {

struct FakeRadio final : M4xWifiConnect::IRadio {
  M4xWifiConnect::RadioStatus nextStatus = M4xWifiConnect::RadioStatus::Connecting;
  bool connected = false;
  int now = 0;
  int disconnects = 0;
  std::vector<std::string> begins;

  bool isConnected() const override { return connected; }
  std::string connectedSsid() const override { return connected ? "Home" : ""; }
  M4xWifiConnect::RadioStatus status() const override {
    return connected ? M4xWifiConnect::RadioStatus::Connected : nextStatus;
  }
  void begin(const std::string& ssid, const std::string&) override {
    begins.push_back(ssid);
    connected = nextStatus == M4xWifiConnect::RadioStatus::Connected;
  }
  void disconnectKeepCreds() override {
    ++disconnects;
    connected = false;
  }
  void setStaMode() override {}
};

M4xWifiConnect::Hooks hooksFor(FakeRadio& radio) {
  M4xWifiConnect::Hooks hooks;
  hooks.nowMs = [&radio] { return static_cast<uint32_t>(radio.now); };
  hooks.sleepMs = [&radio](uint32_t ms) { radio.now += static_cast<int>(ms); };
  return hooks;
}

void testLiveLinkDoesNotNeedSavedCredentials() {
  FakeRadio radio;
  radio.connected = true;
  auto result = M4xWifiConnect::connectSaved(radio, {}, 2000, hooksFor(radio));
  assert(result.ok);
  assert(!result.owned);
  assert(result.error.empty());
}

void testDisconnectedWithoutCredentialsIsNoSavedWifi() {
  FakeRadio radio;
  auto result = M4xWifiConnect::connectSaved(radio, {}, 2000, hooksFor(radio));
  assert(!result.ok);
  assert(result.error == M4xWifiConnect::kErrNoSavedWifi);
}

void testOnlyExplicitAuthFailureInvalidatesCredential() {
  FakeRadio radio;
  radio.nextStatus = M4xWifiConnect::RadioStatus::AuthFailed;
  std::vector<std::string> invalidated;
  auto hooks = hooksFor(radio);
  hooks.onAuthFailure = [&invalidated](const std::string& ssid) {
    invalidated.push_back(ssid);
    return true;
  };
  auto result = M4xWifiConnect::connectSaved(radio, {{"Home", "secret"}}, 2000, hooks);
  assert(!result.ok);
  assert(result.error == M4xWifiConnect::kErrAuthFailed);
  assert(invalidated == std::vector<std::string>{"Home"});
}

void testAuthFailureReportsIfCredentialCouldNotBeInvalidated() {
  FakeRadio radio;
  radio.nextStatus = M4xWifiConnect::RadioStatus::AuthFailed;
  auto hooks = hooksFor(radio);
  hooks.onAuthFailure = [](const std::string&) { return false; };
  const auto result = M4xWifiConnect::connectSaved(radio, {{"Home", "secret"}}, 2000, hooks);
  assert(!result.ok);
  assert(result.credentialInvalidationFailed);
  assert(result.error == M4xWifiConnect::kErrAuthStoreFailed);
}

void testGenericFailureAndTimeoutPreserveCredential() {
  for (const auto status : {M4xWifiConnect::RadioStatus::Failed, M4xWifiConnect::RadioStatus::Connecting}) {
    FakeRadio radio;
    radio.nextStatus = status;
    int invalidations = 0;
    auto hooks = hooksFor(radio);
    hooks.onAuthFailure = [&invalidations](const std::string&) {
      ++invalidations;
      return true;
    };
    const auto result = M4xWifiConnect::connectSaved(radio, {{"Home", "secret"}}, 1000, hooks);
    assert(!result.ok);
    assert(invalidations == 0);
    assert(result.error == (status == M4xWifiConnect::RadioStatus::Failed ? M4xWifiConnect::kErrConnectFailed
                                                                          : M4xWifiConnect::kErrTimeout));
  }
}

void testExistingCrosspointDirDoesNotBlockSave() {
  using namespace wifi_store_test_sd;
  files.clear();
  failWrite = failSync = failRename = failOpenWrite = failMkdir = false;
  auto& store = WIFI_STORE;
  store.clearAll();
  files["/.crosspoint"] = "";
  failMkdir = true;
  assert(store.addCredential("Home", "secret"));
  assert(store.findCredential("Home") != nullptr);
  assert(store.findCredential("Home")->password == "secret");
  failMkdir = false;
}

void testCredentialPersistenceIsTransactionalAndReloadable() {
  using namespace wifi_store_test_sd;
  files.clear();
  failWrite = failSync = failRename = failOpenWrite = failMkdir = false;
  auto& store = WIFI_STORE;
  store.clearAll();
  assert(store.addCredential("Home", "old-secret"));
  assert(store.findCredential("Home") != nullptr);

  failWrite = true;
  assert(!store.addCredential("Home", "new-secret"));
  assert(store.findCredential("Home")->password == "old-secret");
  failWrite = false;

  failSync = true;
  assert(!store.addCredential("Home", "still-old-secret"));
  assert(store.findCredential("Home")->password == "old-secret");
  failSync = false;

  assert(store.loadFromFile());
  assert(store.findCredential("Home")->password == "old-secret");
  assert(store.addCredential("Office", "office-secret"));

  failRename = true;
  assert(!store.addCredential("Cafe", "cafe-secret"));
  assert(store.findCredential("Cafe") == nullptr);
  failRename = false;
  assert(store.loadFromFile());
  assert(store.findCredential("Cafe") == nullptr);
  assert(store.findCredential("Office") != nullptr);
}

}  // namespace

int main() {
  testLiveLinkDoesNotNeedSavedCredentials();
  testDisconnectedWithoutCredentialsIsNoSavedWifi();
  testOnlyExplicitAuthFailureInvalidatesCredential();
  testAuthFailureReportsIfCredentialCouldNotBeInvalidated();
  testGenericFailureAndTimeoutPreserveCredential();
  testExistingCrosspointDirDoesNotBlockSave();
  testCredentialPersistenceIsTransactionalAndReloadable();
  std::cout << "wifi state and credential persistence tests passed\n";
}
