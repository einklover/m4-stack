package com.murphy.m4screenbridge.browser.shell;

/** Pure decision rule for probing a WebView editor after Browser Shell touch dispatch. */
public final class BrowserWebEditorProbePolicy {
    private BrowserWebEditorProbePolicy() {}

    public static boolean shouldProbe(boolean shellEnabled, boolean handled, boolean actionUp,
            boolean panelsVisible, float y, int hostTop, int hostBottom) {
        return shellEnabled
                && handled
                && actionUp
                && !panelsVisible
                && hostBottom > hostTop
                && y >= hostTop
                && y < hostBottom;
    }
}
