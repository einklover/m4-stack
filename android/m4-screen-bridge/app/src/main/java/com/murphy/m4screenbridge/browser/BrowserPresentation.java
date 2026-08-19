package com.murphy.m4screenbridge.browser;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.view.Display;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

/** Minimal Chromium/WebView surface hosted entirely on the M4 virtual display. */
final class BrowserPresentation extends Presentation {
    interface Listener {
        void onPageStarted(String url);
        void onPageFinished(String url);
        void onPageProgress(int progress);
        void onPageTitle(String title);
        void onPageError(String url, String description);
    }

    private final String initialUrl;
    private final Listener listener;
    private final JsProbe jsProbe = new JsProbe();
    private WebView webView;

    BrowserPresentation(Context context, Display display, String initialUrl, Listener listener) {
        super(context, display);
        this.initialUrl = initialUrl;
        this.listener = listener;
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

        webView.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                if (listener != null) listener.onPageStarted(url == null ? "" : url);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                if (listener != null) listener.onPageFinished(url == null ? "" : url);
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request == null || !request.isForMainFrame() || listener == null) return;
                String url = request.getUrl() == null ? "" : request.getUrl().toString();
                CharSequence description = error == null ? null : error.getDescription();
                listener.onPageError(url, description == null ? "" : description.toString());
            }
        });
        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public void onProgressChanged(WebView view, int newProgress) {
                if (listener != null) listener.onPageProgress(Math.max(0, Math.min(100, newProgress)));
            }

            @Override
            public void onReceivedTitle(WebView view, String title) {
                if (listener != null) listener.onPageTitle(title == null ? "" : title);
            }
        });
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
        // Do not navigate to about:blank during teardown: navigation callbacks are product state
        // and must not overwrite the last real URL that BrowserBridgeService will restore.
        webView.stopLoading();
        webView.removeJavascriptInterface("M4Input");
        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WebChromeClient());
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
