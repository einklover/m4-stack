#pragma once

#include "apps/M4xManifest.h"
#include "apps/M4xRegistry.h"

#include <string>

struct M4xInstallResult {
  bool ok = false;
  std::string error;       // machine key
  std::string message;     // human-readable
  M4xManifest manifest;
  std::string installPath;
};

class M4xInstaller {
 public:
  // Read package, parse manifest only (no write). packagePath is absolute SD path.
  static M4xInstallResult probe(const std::string& packagePath);

  // Install or upgrade package. Overwrites same id if versionCode >= installed.
  static M4xInstallResult install(const std::string& packagePath);

  // Uninstall by id. clearData removes /apps_data/<id>.
  static bool uninstall(const std::string& id, bool clearData, std::string& errorOut);

  // Absolute path to app entry script.
  static std::string entryScriptPath(const M4xInstalledApp& app);

  // Ensure base directories exist.
  static void ensureLayout();
};
