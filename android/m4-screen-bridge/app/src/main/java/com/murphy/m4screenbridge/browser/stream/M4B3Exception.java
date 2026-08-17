package com.murphy.m4screenbridge.browser.stream;

/** Deterministic M4B3 parse/apply failure. Never used to swallow a partial apply. */
public final class M4B3Exception extends RuntimeException {
    public enum Kind {
        TRUNCATED,
        OVERSIZED,
        OVERFLOW,
        INVALID,
        VERSION
    }

    public final Kind kind;

    public M4B3Exception(Kind kind, String message) {
        super(kind + ": " + message);
        this.kind = kind;
    }

    public static M4B3Exception truncated(String message) {
        return new M4B3Exception(Kind.TRUNCATED, message);
    }

    public static M4B3Exception oversized(String message) {
        return new M4B3Exception(Kind.OVERSIZED, message);
    }

    public static M4B3Exception overflow(String message) {
        return new M4B3Exception(Kind.OVERFLOW, message);
    }

    public static M4B3Exception invalid(String message) {
        return new M4B3Exception(Kind.INVALID, message);
    }

    public static M4B3Exception version(String message) {
        return new M4B3Exception(Kind.VERSION, message);
    }
}
