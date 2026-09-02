#include "navigation/M4NavigationSupervisor.h"

void M4NavigationSupervisor::attach() {
    detached_ = false;
    callbackAllowed_ = true;
}

void M4NavigationSupervisor::detach() {
    // Idempotent detach: repeated teardown must converge on the same safe state.
    if (detached_) {
        callbackAllowed_ = false;
        return;
    }

    callbackAllowed_ = false;
    detached_ = true;
}

void M4NavigationSupervisor::teardown() {
    detach();
}

bool M4NavigationSupervisor::isDetached() const {
    return detached_;
}

bool M4NavigationSupervisor::canDispatchCallback() const {
    return callbackAllowed_ && !detached_;
}
