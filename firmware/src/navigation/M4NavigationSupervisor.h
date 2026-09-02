#pragma once

class M4NavigationSupervisor {
public:
    void attach();
    void detach();
    void teardown();
    bool isDetached() const;

private:
    bool detached_{true};
    bool callbackAllowed_{false};
};
