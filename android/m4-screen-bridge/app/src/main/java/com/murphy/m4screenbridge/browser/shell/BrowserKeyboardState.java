package com.murphy.m4screenbridge.browser.shell;

import java.util.Locale;

/** Pure-Java text/editing state for the app-owned M4 browser keyboard. */
public final class BrowserKeyboardState {
    public enum Mode { LETTERS, SYMBOLS }

    private final StringBuilder text = new StringBuilder();
    private Mode mode = Mode.LETTERS;
    private boolean shifted;

    public String text() {
        return text.toString();
    }

    public Mode mode() {
        return mode;
    }

    public boolean shifted() {
        return shifted;
    }

    public void append(String value) {
        if (value == null || value.isEmpty()) return;
        String output = shifted ? value.toUpperCase(Locale.ROOT) : value;
        text.append(output);
        if (shifted) shifted = false;
    }

    public void backspace() {
        int length = text.length();
        if (length == 0) return;
        int codePoint = text.codePointBefore(length);
        text.delete(length - Character.charCount(codePoint), length);
    }

    public void space() {
        text.append(' ');
    }

    public void toggleShift() {
        if (mode == Mode.LETTERS) shifted = !shifted;
    }

    public void toggleMode() {
        mode = mode == Mode.LETTERS ? Mode.SYMBOLS : Mode.LETTERS;
        shifted = false;
    }

    public void clear() {
        text.setLength(0);
        shifted = false;
    }

    public void replace(String value) {
        text.setLength(0);
        if (value != null) text.append(value);
        shifted = false;
    }
}
