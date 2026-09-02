#include "navigation/M4NavigationSupervisor.h"

void M4NavigationSupervisor::attach() {
    detached_ = false;
    callbackAllowed_ = true;
}

void M4NavigationSupervisor::detach() {
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
