package com.murphy.m4screenbridge;

import android.content.SharedPreferences;

/** Tunables and Browser Bridge product state read from SharedPreferences with safe clamping. */
public final class Prefs {
    public static final String KEY_THRESHOLD = "threshold";
    public static final String KEY_DITHER = "dither";
    public static final String KEY_MAX_GAP = "max_gap";
    public static final String KEY_PREFETCH_MS = "prefetch_ms";
    public static final String KEY_CROP = "crop_mode";
    public static final String KEY_CACHE_ENABLED = "cache_enabled";
    public static final String KEY_M4B3_HOST = "m4b3_host";
    public static final String KEY_M4B3_PORT = "m4b3_port";
    public static final String KEY_M4B3_CACHED_HOST = "m4b3_cached_host";
    public static final String KEY_M4B3_CACHED_PORT = "m4b3_cached_port";
    public static final String KEY_BROWSER_RESUME_ENABLED = "browser_resume_enabled";
    public static final String KEY_BROWSER_LAST_URL = "browser_last_url";
    public static final int DEF_M4B3_PORT = 48624;

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

    public static String m4b3Host(SharedPreferences sp) {
        String host = sp.getString(KEY_M4B3_HOST, "");
        if (host == null) return "";
        host = host.trim();
        if (host.isEmpty() || "loopback".equalsIgnoreCase(host) || "local".equalsIgnoreCase(host)) {
            return "";
        }
        return host;
    }

    public static String m4b3HostRaw(SharedPreferences sp) {
        String host = sp.getString(KEY_M4B3_HOST, "");
        return host == null ? "" : host.trim();
    }

    public static int m4b3Port(SharedPreferences sp) {
        return clamp(sp.getInt(KEY_M4B3_PORT, DEF_M4B3_PORT), 1, 65535);
    }

    public static String cachedHost(SharedPreferences sp) {
        String host = sp.getString(KEY_M4B3_CACHED_HOST, "");
        return host == null ? "" : host.trim();
    }

    public static int cachedPort(SharedPreferences sp) {
        return clamp(sp.getInt(KEY_M4B3_CACHED_PORT, DEF_M4B3_PORT), 1, 65535);
    }

    public static void storeCachedEndpoint(SharedPreferences sp, String host, int port) {
        if (sp == null || host == null || host.trim().isEmpty() || port < 1 || port > 65535) return;
        sp.edit().putString(KEY_M4B3_CACHED_HOST, host.trim()).putInt(KEY_M4B3_CACHED_PORT, port).apply();
    }

    public static void clearCachedEndpoint(SharedPreferences sp) {
        if (sp == null) return;
        sp.edit().remove(KEY_M4B3_CACHED_HOST).remove(KEY_M4B3_CACHED_PORT).apply();
    }

    public static boolean browserResumeEnabled(SharedPreferences sp) {
        return sp != null && sp.getBoolean(KEY_BROWSER_RESUME_ENABLED, false);
    }

    public static String browserLastUrl(SharedPreferences sp) {
        if (sp == null) return "";
        String url = sp.getString(KEY_BROWSER_LAST_URL, "");
        return url == null ? "" : url.trim();
    }

    public static void setBrowserResumeEnabled(SharedPreferences sp, boolean enabled) {
        if (sp == null) return;
        sp.edit().putBoolean(KEY_BROWSER_RESUME_ENABLED, enabled).apply();
    }

    /** Persist only real browser destinations; lab data: pages must never replace product state. */
    public static void storeBrowserLastUrl(SharedPreferences sp, String rawUrl) {
        if (sp == null || rawUrl == null) return;
        String url = rawUrl.trim();
        String lower = url.toLowerCase(java.util.Locale.ROOT);
        if (!(lower.startsWith("http://") || lower.startsWith("https://")
                || "about:blank".equals(lower))) {
            return;
        }
        sp.edit().putString(KEY_BROWSER_LAST_URL, url).apply();
    }

    private static int clamp(int v, int lo, int hi) {
        return Math.max(lo, Math.min(hi, v));
    }
}
