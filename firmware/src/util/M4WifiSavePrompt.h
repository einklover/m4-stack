#pragma once

#include <string>

// Phase 2A Task 2: explicit save-prompt decision for new Wi-Fi credentials
// (dependency-free).
//
// A newly entered credential must never auto-save: after a successful
// connection the activity shows an explicit Yes/No prompt, and only Yes
// reaches WifiCredentialStore. No keeps the already-established session
// usable without persistence. Saved/auto-connect and open-network successes
// bypass the prompt, and failures never produce one (the decision is only
// consulted on the connect-success path).
enum class M4WifiSavePrompt { Needed, NotNeeded };

// Prompt iff this success used a freshly entered (non-saved) password.
inline M4WifiSavePrompt m4WifiSavePromptDecision(bool usedSavedPassword,
                                                 const std::string& enteredPassword) {
  if (!usedSavedPassword && !enteredPassword.empty()) return M4WifiSavePrompt::Needed;
  return M4WifiSavePrompt::NotNeeded;
}

// Persist iff the prompt was needed and the user explicitly said Yes.
inline bool m4WifiSaveConfirmed(M4WifiSavePrompt prompt, bool userSaidYes) {
  return prompt == M4WifiSavePrompt::Needed && userSaidYes;
}
