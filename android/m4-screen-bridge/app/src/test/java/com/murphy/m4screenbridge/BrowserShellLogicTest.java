package com.murphy.m4screenbridge;

import com.murphy.m4screenbridge.browser.shell.BrowserAddressResolver;
import com.murphy.m4screenbridge.browser.shell.BrowserKeyboardState;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;

public final class BrowserShellLogicTest {
    public static void main(String[] args) {
        addressResolution();
        keyboardEditing();
        keyboardTargetRouting();
        webEditorProbePolicy();
        webEditorProbeRetryLifecycle();
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

    private static void keyboardTargetRouting() {
        try {
            Class<?> routerClass = Class.forName(
                    "com.murphy.m4screenbridge.browser.shell.BrowserKeyboardRouter");
            Class<?> targetClass = Class.forName(
                    "com.murphy.m4screenbridge.browser.shell.BrowserKeyboardRouter$Target");
            Object router = routerClass.getConstructor().newInstance();
            Method setTarget = routerClass.getMethod("setTarget", targetClass);
            Method clearTarget = routerClass.getMethod("clearTarget");
            Method commitText = routerClass.getMethod("commitText", String.class);
            Method backspace = routerClass.getMethod("backspace");
            Method submit = routerClass.getMethod("submit");

            List<String> events = new ArrayList<>();
            Object omnibox = recordingTarget(targetClass, events, "omnibox");
            Object web = recordingTarget(targetClass, events, "web");

            setTarget.invoke(router, omnibox);
            yes((Boolean) commitText.invoke(router, "a"));
            yes((Boolean) backspace.invoke(router));
            yes((Boolean) submit.invoke(router));

            setTarget.invoke(router, web);
            yes((Boolean) commitText.invoke(router, "中"));
            yes((Boolean) submit.invoke(router));

            clearTarget.invoke(router);
            no((Boolean) commitText.invoke(router, "ignored"));
            no((Boolean) backspace.invoke(router));
            no((Boolean) submit.invoke(router));

            eq("omnibox:text:a,omnibox:backspace,omnibox:submit,web:text:中,web:submit",
                    String.join(",", events));
        } catch (ClassNotFoundException expectedRed) {
            throw new AssertionError("shared keyboard target router is missing", expectedRed);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("shared keyboard target router API mismatch", e);
        }
    }

    private static void webEditorProbePolicy() {
        try {
            Class<?> policyClass = Class.forName(
                    "com.murphy.m4screenbridge.browser.shell.BrowserWebEditorProbePolicy");
            Method shouldProbe = policyClass.getMethod("shouldProbe",
                    boolean.class, boolean.class, boolean.class, boolean.class,
                    float.class, int.class, int.class);

            yes((Boolean) shouldProbe.invoke(null, true, true, true, false, 320f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, true, true, true, true, 320f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, true, true, true, false, 20f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, true, true, true, false, 760f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, false, true, true, false, 320f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, true, false, true, false, 320f, 52, 744));
            no((Boolean) shouldProbe.invoke(null, true, true, false, false, 320f, 52, 744));
        } catch (ClassNotFoundException expectedRed) {
            throw new AssertionError("web editor probe policy is missing", expectedRed);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("web editor probe policy API mismatch", e);
        }
    }

    private static void webEditorProbeRetryLifecycle() {
        try {
            Class<?> retryClass = Class.forName(
                    "com.murphy.m4screenbridge.browser.shell.BrowserWebEditorProbeRetry");
            Object retry = retryClass.getConstructor().newInstance();
            Method begin = retryClass.getMethod("begin");
            Method invalidate = retryClass.getMethod("invalidate");
            Method isCurrent = retryClass.getMethod("isCurrent", long.class);
            Method delayMs = retryClass.getMethod("delayMs", int.class);
            Method hasNext = retryClass.getMethod("hasNext", int.class);

            long first = (Long) begin.invoke(retry);
            yes((Boolean) isCurrent.invoke(retry, first));
            eq(0L, delayMs.invoke(null, 0));
            yes((Boolean) hasNext.invoke(null, 0));

            long second = (Long) begin.invoke(retry);
            no((Boolean) isCurrent.invoke(retry, first));
            yes((Boolean) isCurrent.invoke(retry, second));
            long retryDelay = (Long) delayMs.invoke(null, 1);
            yes(retryDelay > 0L);
            yes((Boolean) hasNext.invoke(null, 1));

            invalidate.invoke(retry);
            no((Boolean) isCurrent.invoke(retry, second));

            int attempt = 0;
            long lastDelay = -1L;
            while (true) {
                long delay = (Long) delayMs.invoke(null, attempt);
                yes(delay >= 0L);
                yes(delay >= lastDelay);
                lastDelay = delay;
                boolean next = (Boolean) hasNext.invoke(null, attempt);
                if (!next) break;
                attempt++;
                if (attempt > 8) throw new AssertionError("web editor retry schedule is unbounded");
            }
            yes(attempt >= 2);
        } catch (ClassNotFoundException expectedRed) {
            throw new AssertionError("web editor probe retry lifecycle is missing", expectedRed);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("web editor probe retry API mismatch", e);
        }
    }

    private static Object recordingTarget(Class<?> targetClass, List<String> events, String name) {
        return Proxy.newProxyInstance(targetClass.getClassLoader(), new Class<?>[] {targetClass},
                (proxy, method, args) -> {
                    switch (method.getName()) {
                        case "commitText":
                            events.add(name + ":text:" + args[0]);
                            return null;
                        case "backspace":
                            events.add(name + ":backspace");
                            return null;
                        case "submit":
                            events.add(name + ":submit");
                            return null;
                        default:
                            throw new AssertionError("unexpected target method " + method.getName());
                    }
                });
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
