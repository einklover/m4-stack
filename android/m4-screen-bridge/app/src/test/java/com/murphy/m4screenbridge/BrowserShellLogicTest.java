package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.shell.BrowserAddressResolver;
import com.murphy.m4screenbridge.browser.shell.BrowserKeyboardState;

public final class BrowserShellLogicTest {
    public static void main(String[] args) {
        addressResolution();
        keyboardEditing();
        System.out.println("BrowserShellLogicTest PASS");
    }

    private static void addressResolution() {
        eq("https://example.com", BrowserAddressResolver.resolve("example.com",
                "https://duckduckgo.com/?q=%s"));
        eq("https://example.com/a", BrowserAddressResolver.resolve("https://example.com/a",
                "https://duckduckgo.com/?q=%s"));
        eq("about:blank", BrowserAddressResolver.resolve("", "https://duckduckgo.com/?q=%s"));
        eq("https://duckduckgo.com/?q=hello+world",
                BrowserAddressResolver.resolve("hello world", "https://duckduckgo.com/?q=%s"));
        eq("https://duckduckgo.com/?q=%E4%B8%AD%E6%96%87+%E6%90%9C%E7%B4%A2",
                BrowserAddressResolver.resolve("中文 搜索", "https://duckduckgo.com/?q=%s"));
        eq("https://duckduckgo.com/?q=term",
                BrowserAddressResolver.resolve("term", ""));
    }

    private static void keyboardEditing() {
        BrowserKeyboardState k = new BrowserKeyboardState();
        eq(BrowserKeyboardState.Mode.LETTERS, k.mode());
        no(k.shifted());
        k.append("a");
        eq("a", k.text());
        k.toggleShift();
        yes(k.shifted());
        k.append("b");
        eq("aB", k.text());
        no(k.shifted());
        k.space();
        k.append("c");
        eq("aB c", k.text());
        k.backspace();
        eq("aB ", k.text());
        k.toggleMode();
        eq(BrowserKeyboardState.Mode.SYMBOLS, k.mode());
        k.replace("example.com");
        eq("example.com", k.text());
        k.clear();
        eq("", k.text());

        k.replace("A😀");
        k.backspace();
        eq("A", k.text());
    }

    private static void yes(boolean value) {
        if (!value) throw new AssertionError("expected true");
    }

    private static void no(boolean value) {
        if (value) throw new AssertionError("expected false");
    }

    private static void eq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected=" + expected + " actual=" + actual);
        }
    }
}
