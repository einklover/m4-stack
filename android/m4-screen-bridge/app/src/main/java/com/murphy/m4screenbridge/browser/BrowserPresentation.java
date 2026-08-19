package com.murphy.m4screenbridge.browser;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.view.Display;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.MotionEvent;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** Minimal Chromium/WebView surface hosted entirely on the M4 virtual display. */
final class BrowserPresentation extends Presentation {
    private final String initialUrl;
    private final JsProbe jsProbe = new JsProbe();
    private WebView webView;

    BrowserPresentation(Context context, Display display, String initialUrl) {
        super(context, display);
        this.initialUrl = initialUrl;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        if (window != null) {
            window.setFormat(PixelFormat.OPAQUE);
            WindowManager.LayoutParams lp = window.getAttributes();
            lp.screenBrightness = 1.0f;
            lp.dimAmount = 0f;
            window.setAttributes(lp);
        }

        webView = new WebView(getContext());
        webView.setBackgroundColor(Color.WHITE);
        webView.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        WebSettings settings = webView.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setLoadsImagesAutomatically(true);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(false);
        settings.setBuiltInZoomControls(false);
        settings.setDisplayZoomControls(false);
        settings.setSupportZoom(true);

        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WebChromeClient());
        webView.addJavascriptInterface(jsProbe, "M4Input");
        setContentView(webView);
        webView.loadUrl(initialUrl);
    }

    void loadUrl(String url) {
        if (webView != null) webView.loadUrl(url);
    }

    boolean dispatchBrowserTouch(MotionEvent event) {
        return webView != null && event != null && webView.dispatchTouchEvent(event);
    }

    boolean goBackInBrowser() {
        if (webView == null || !webView.canGoBack()) return false;
        webView.goBack();
        return true;
    }

    boolean reloadBrowser() {
        if (webView == null) return false;
        webView.reload();
        return true;
    }

    JsProbe jsProbe() {
        return jsProbe;
    }

    void destroyBrowser() {
        if (webView == null) return;
        webView.stopLoading();
        webView.loadUrl("about:blank");
        webView.clearHistory();
        webView.removeAllViews();
        webView.destroy();
        webView = null;
    }

    /** Filled by the deterministic input page through addJavascriptInterface. */
    static final class JsProbe {
        volatile int buttonA;
        volatile int down;
        volatile int move;
        volatile int up;
        volatile int cancel;
        volatile int lastX;
        volatile int lastY;
        volatile String lastAction = "";
        volatile String lastHit = "";
        volatile int scrollY;
        volatile int longPress;
        volatile int hits;
        volatile String lastLog = "";

        @JavascriptInterface
        public void report(String action, int x, int y, String hit, int buttonAClicks,
                int scroll, int longPressCount, int downN, int moveN, int upN, int cancelN) {
            lastAction = action == null ? "" : action;
            lastX = x;
            lastY = y;
            lastHit = hit == null ? "" : hit;
            buttonA = buttonAClicks;
            scrollY = scroll;
            longPress = longPressCount;
            down = downN;
            move = moveN;
            up = upN;
            cancel = cancelN;
            hits++;
            lastLog = lastAction + " " + lastX + "," + lastY + " hit=" + lastHit
                    + " btnA=" + buttonA + " scrollY=" + scrollY + " lp=" + longPress;
        }
    }
}
