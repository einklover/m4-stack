#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
reader = (ROOT / "firmware/src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
subactivity = (ROOT / "firmware/src/activities/ActivityWithSubactivity.cpp").read_text(encoding="utf-8")
activity = (ROOT / "firmware/src/activities/Activity.h").read_text(encoding="utf-8")
main = (ROOT / "firmware/src/main.cpp").read_text(encoding="utf-8")


def body(signature: str) -> str:
    start = reader.find(signature)
    assert start >= 0, f"missing function: {signature}"
    brace = reader.find("{", start)
    assert brace >= 0, f"missing body: {signature}"
    depth = 0
    for index in range(brace, len(reader)):
        if reader[index] == "{":
            depth += 1
        elif reader[index] == "}":
            depth -= 1
            if depth == 0:
                return reader[brace : index + 1]
    raise AssertionError(f"unterminated body: {signature}")


snapshot = body("void TxtReaderActivity::persistProgressSnapshot(const ProgressSnapshot& snapshot) const")
assert "xSemaphoreTake(progressWriterMutex_" in snapshot
assert "renderingMutex" not in snapshot and "lockState(" not in snapshot
assert "SdMan.remove" in snapshot and "SdMan.rename" in snapshot

save = body("void TxtReaderActivity::saveProgress() const")
assert save.find("unlockState()") < save.find("persistProgressSnapshot(snapshot)")

history = body("void TxtReaderActivity::persistOpenHistory()")
assert history.find("unlockState()") < history.find("xSemaphoreTake(progressWriterMutex_")
assert history.find("xSemaphoreTake(progressWriterMutex_") < history.find("APP_STATE.saveToFile()")
assert "progressGeneration_" in history

exit_body = body("void TxtReaderActivity::onExit()")
assert "kExitWaitMs" in exit_body and "deadline" in exit_body
assert "portMAX_DELAY" not in exit_body

# Runtime TTF faces are global renderer resources. Reaping one Reader must not
# release them while another top-level or nested Reader still owns a task.
assert "virtual bool hasLiveReaderOwner() const" in activity
assert "bool ActivityWithSubactivity::hasLiveReaderOwner() const" in subactivity
assert "Activity::hasLiveReaderOwner()" in subactivity
assert "subActivity->hasLiveReaderOwner()" in subactivity
assert "child->hasLiveReaderOwner()" in subactivity
assert "static bool hasPendingReaderOwner()" in main
assert "!hasPendingReaderOwner()" in main

print("reader persistence outside render lock contract: PASS")
