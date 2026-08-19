package com.murphy.m4screenbridge.browser.shell;

import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.util.Locale;

/** Pure-Java omnibox classifier used by the M4 browser shell. */
public final class BrowserAddressResolver {
    public static final String DEFAULT_SEARCH_TEMPLATE = "https://duckduckgo.com/?q=%s";

    private BrowserAddressResolver() {}

    public static String resolve(String raw, String searchTemplate) {
        String input = raw == null ? "" : raw.trim();
        if (input.isEmpty()) return "about:blank";

        String lower = input.toLowerCase(Locale.ROOT);
        if (lower.startsWith("http://") || lower.startsWith("https://")
                || lower.startsWith("about:") || lower.startsWith("data:")) {
            return input;
        }

        if (looksLikeHost(input)) return "https://" + input;

        String template = searchTemplate == null ? "" : searchTemplate.trim();
        if (template.isEmpty() || !template.contains("%s")) {
            template = DEFAULT_SEARCH_TEMPLATE;
        }
        return template.replace("%s", encode(input));
    }

    private static boolean looksLikeHost(String input) {
        for (int i = 0; i < input.length(); i++) {
            if (Character.isWhitespace(input.charAt(i))) return false;
        }
        return "localhost".equalsIgnoreCase(input) || input.indexOf('.') >= 0;
    }

    private static String encode(String input) {
        try {
            return URLEncoder.encode(input, "UTF-8");
        } catch (UnsupportedEncodingException impossible) {
            throw new IllegalStateException("UTF-8 unavailable", impossible);
        }
    }
}
