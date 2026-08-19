package com.murphy.m4screenbridge.browser.shell;

/** Routes app-owned keyboard editing actions to the currently focused browser editor. */
public final class BrowserKeyboardRouter {
    public interface Target {
        void commitText(String text);
        void backspace();
        void submit();
    }

    private Target target;

    public void setTarget(Target target) {
        this.target = target;
    }

    public void clearTarget() {
        target = null;
    }

    public boolean commitText(String text) {
        Target current = target;
        if (current == null) return false;
        current.commitText(text == null ? "" : text);
        return true;
    }

    public boolean backspace() {
        Target current = target;
        if (current == null) return false;
        current.backspace();
        return true;
    }

    public boolean submit() {
        Target current = target;
        if (current == null) return false;
        current.submit();
        return true;
    }
}
