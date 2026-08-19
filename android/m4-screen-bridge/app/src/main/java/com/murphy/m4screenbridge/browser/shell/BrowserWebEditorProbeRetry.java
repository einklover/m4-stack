package com.murphy.m4screenbridge.browser.shell;

/** Bounded, cancellable schedule for probing a WebView editor after touch. */
public final class BrowserWebEditorProbeRetry {
    private static final long[] DELAYS_MS = {0L, 40L, 120L, 300L};
    private long generation;

    public long begin() {
        return ++generation;
    }

    public void invalidate() {
        generation++;
    }

    public boolean isCurrent(long token) {
        return token == generation;
    }

    public static long delayMs(int attempt) {
        if (attempt < 0 || attempt >= DELAYS_MS.length) {
            throw new IllegalArgumentException("invalid web editor probe attempt " + attempt);
        }
        return DELAYS_MS[attempt];
    }

    public static boolean hasNext(int attempt) {
        return attempt >= 0 && attempt + 1 < DELAYS_MS.length;
    }
}
