package com.murphy.m4screenbridge;

import android.content.SharedPreferences;

/** Tunables read from SharedPreferences with safe clamping. */
public final class Prefs {
    public static final String KEY_THRESHOLD = "threshold";
    public static final String KEY_DITHER = "dither";
    public static final String KEY_MAX_GAP = "max_gap";
    public static final String KEY_PREFETCH_MS = "prefetch_ms";
    public static final String KEY_CROP = "crop_mode";
    public static final String KEY_CACHE_ENABLED = "cache_enabled";

    public static final int DEF_THRESHOLD = 128;
    public static final int DEF_MAX_GAP = 12;
    public static final int DEF_PREFETCH_MS = 900;

    public final int threshold;
    public final boolean dither;
    public final int maxGap;
    public final int prefetchMs;
    public final String cropMode;
    public final boolean cacheEnabled;

    public Prefs(SharedPreferences sp) {
        threshold = clamp(sp.getInt(KEY_THRESHOLD, DEF_THRESHOLD), 0, 255);
        dither = sp.getBoolean(KEY_DITHER, false);
        maxGap = clamp(sp.getInt(KEY_MAX_GAP, DEF_MAX_GAP), 0, 80);
        prefetchMs = clamp(sp.getInt(KEY_PREFETCH_MS, DEF_PREFETCH_MS), 100, 5000);
        String c = sp.getString(KEY_CROP, "fit");
        cropMode = "cover".equals(c) ? "cover" : "fit";
        cacheEnabled = sp.getBoolean(KEY_CACHE_ENABLED, true);
    }

    private static int clamp(int v, int lo, int hi) {
        return Math.max(lo, Math.min(hi, v));
    }
}
