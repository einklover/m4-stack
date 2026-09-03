#pragma once

class M4NavigationSupervisor {
public:
    void attach();
    void detach();
    void teardown();
    bool isDetached() const;
    bool canDispatchCallback() const;

private:
    bool detached_{true};
    bool callbackAllowed_{false};
};
