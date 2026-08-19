package com.murphy.m4screenbridge.browser;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Display;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import com.murphy.m4screenbridge.browser.shell.BrowserAddressResolver;
import com.murphy.m4screenbridge.browser.shell.BrowserShellStyle;

/** Chromium/WebView surface plus a static E-ink browser shell on the M4 virtual display. */
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
    private final Handler statusHandler = new Handler(Looper.getMainLooper());
    private final Runnable statusTick = new Runnable() {
        @Override
        public void run() {
            if (webView == null) return;
            setBridgeStatus(BrowserBridgeService.bridgeOverlayStatus());
            statusHandler.postDelayed(this, 750);
        }
    };

    private FrameLayout shellRoot;
    private WebView webView;
    private EditText omnibox;
    private TextView bridgeStatus;
    private LinearLayout tabsPanel;
    private LinearLayout menuPanel;
    private TextView tabsCurrent;
    private String bridgeStatusText = "";
    private String currentPageUrl = "";
    private String homepage = "about:blank";
    private String searchTemplate = BrowserAddressResolver.DEFAULT_SEARCH_TEMPLATE;

    BrowserPresentation(Context context, Display display, String initialUrl, Listener listener) {
        super(context, display);
        this.initialUrl = initialUrl;
        this.listener = listener;
        this.currentPageUrl = initialUrl == null ? "" : initialUrl;
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

        shellRoot = new FrameLayout(getContext());
        shellRoot.setBackgroundColor(BrowserShellStyle.WHITE);

        LinearLayout column = new LinearLayout(getContext());
        column.setOrientation(LinearLayout.VERTICAL);
        column.setBackgroundColor(BrowserShellStyle.WHITE);
        shellRoot.addView(column, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        column.addView(buildOmniboxRow(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, BrowserShellStyle.OMNIBOX_HEIGHT));

        FrameLayout webHost = new FrameLayout(getContext());
        webHost.setBackgroundColor(BrowserShellStyle.WHITE);
        LinearLayout.LayoutParams webHostLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        column.addView(webHost, webHostLp);

        webView = new WebView(getContext());
        webView.setBackgroundColor(Color.WHITE);
        webHost.addView(webView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        configureWebView();

        column.addView(buildToolbar(), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, BrowserShellStyle.TOOLBAR_HEIGHT));

        tabsPanel = buildTabsPanel();
        shellRoot.addView(tabsPanel, panelLayoutParams());
        menuPanel = buildMenuPanel();
        shellRoot.addView(menuPanel, panelLayoutParams());

        bridgeStatus = new TextView(getContext());
        bridgeStatus.setBackgroundColor(BrowserShellStyle.BLACK);
        bridgeStatus.setTextColor(BrowserShellStyle.WHITE);
        bridgeStatus.setTextSize(14);
        bridgeStatus.setGravity(Gravity.CENTER_VERTICAL);
        bridgeStatus.setPadding(12, 0, 12, 0);
        bridgeStatus.setSingleLine(true);
        bridgeStatus.setClickable(false);
        bridgeStatus.setFocusable(false);
        bridgeStatus.setVisibility(View.GONE);
        FrameLayout.LayoutParams statusLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, BrowserShellStyle.STATUS_HEIGHT, Gravity.TOP);
        statusLp.topMargin = BrowserShellStyle.OMNIBOX_HEIGHT;
        shellRoot.addView(bridgeStatus, statusLp);

        setContentView(shellRoot);
        syncOmnibox(initialUrl);
        webView.loadUrl(initialUrl);
        statusHandler.post(statusTick);
    }

    private View buildOmniboxRow() {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(1, 1, 1, 1);
        row.setBackgroundColor(BrowserShellStyle.BLACK);

        omnibox = new EditText(getContext());
        omnibox.setSingleLine(true);
        omnibox.setTextSize(15);
        omnibox.setTextColor(BrowserShellStyle.BLACK);
        omnibox.setHintTextColor(Color.DKGRAY);
        omnibox.setBackgroundColor(BrowserShellStyle.WHITE);
        omnibox.setHint("Address or search");
        omnibox.setPadding(10, 0, 8, 0);
        omnibox.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        omnibox.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        omnibox.setShowSoftInputOnFocus(false);
        omnibox.setOnEditorActionListener((view, actionId, event) -> {
            boolean enter = event != null && event.getAction() == KeyEvent.ACTION_UP
                    && event.getKeyCode() == KeyEvent.KEYCODE_ENTER;
            if (actionId == EditorInfo.IME_ACTION_GO || enter) {
                submitOmnibox();
                return true;
            }
            return false;
        });
        row.addView(omnibox, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.MATCH_PARENT, 1f));

        TextView go = makeAction("Go", this::submitOmnibox);
        row.addView(go, new LinearLayout.LayoutParams(58,
                ViewGroup.LayoutParams.MATCH_PARENT));
        return row;
    }

    private View buildToolbar() {
        LinearLayout toolbar = new LinearLayout(getContext());
        toolbar.setOrientation(LinearLayout.HORIZONTAL);
        toolbar.setBackgroundColor(BrowserShellStyle.BLACK);

        addToolbarAction(toolbar, "←", this::goBackInBrowser);
        addToolbarAction(toolbar, "→", this::goForwardInBrowser);
        addToolbarAction(toolbar, "⌂", this::goHomeInBrowser);
        addToolbarAction(toolbar, "Tabs", this::toggleTabsPanel);
        addToolbarAction(toolbar, "↻", this::reloadBrowser);
        addToolbarAction(toolbar, "⋮", this::toggleMenuPanel);
        return toolbar;
    }

    private LinearLayout buildTabsPanel() {
        LinearLayout panel = makePanel();
        tabsCurrent = makePanelLabel("Current tab");
        panel.addView(tabsCurrent, rowLayoutParams());
        panel.addView(makeAction("+ New tab", () -> {
            hidePanels();
            if (webView != null) webView.loadUrl(homepage);
        }), rowLayoutParams());
        panel.addView(makeAction("Close tabs panel", this::hidePanels), rowLayoutParams());
        return panel;
    }

    private LinearLayout buildMenuPanel() {
        LinearLayout panel = makePanel();
        panel.addView(makeAction("Home", () -> {
            hidePanels();
            goHomeInBrowser();
        }), rowLayoutParams());
        panel.addView(makeAction("Focus address", () -> {
            hidePanels();
            focusOmnibox();
        }), rowLayoutParams());
        panel.addView(makeAction("Reload", () -> {
            hidePanels();
            reloadBrowser();
        }), rowLayoutParams());
        panel.addView(makeAction("Close menu", this::hidePanels), rowLayoutParams());
        return panel;
    }

    private LinearLayout makePanel() {
        LinearLayout panel = new LinearLayout(getContext());
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(BrowserShellStyle.WHITE);
        panel.setPadding(1, 1, 1, 1);
        panel.setVisibility(View.GONE);
        return panel;
    }

    private FrameLayout.LayoutParams panelLayoutParams() {
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
        lp.bottomMargin = BrowserShellStyle.TOOLBAR_HEIGHT;
        return lp;
    }

    private LinearLayout.LayoutParams rowLayoutParams() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, BrowserShellStyle.PANEL_ROW_HEIGHT);
    }

    private TextView makePanelLabel(String text) {
        TextView label = new TextView(getContext());
        label.setText(text);
        label.setTextColor(BrowserShellStyle.BLACK);
        label.setBackgroundColor(BrowserShellStyle.WHITE);
        label.setTextSize(15);
        label.setGravity(Gravity.CENTER_VERTICAL);
        label.setPadding(12, 0, 12, 0);
        return label;
    }

    private TextView makeAction(String text, Runnable action) {
        TextView button = new TextView(getContext());
        button.setText(text);
        button.setTextColor(BrowserShellStyle.WHITE);
        button.setBackgroundColor(BrowserShellStyle.BLACK);
        button.setTextSize(14);
        button.setGravity(Gravity.CENTER);
        button.setClickable(true);
        button.setFocusable(true);
        button.setOnClickListener(v -> action.run());
        return button;
    }

    private void addToolbarAction(LinearLayout toolbar, String text, Runnable action) {
        TextView button = makeAction(text, action);
        toolbar.addView(button, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.MATCH_PARENT, 1f));
    }

    private void configureWebView() {
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
                currentPageUrl = url == null ? "" : url;
                syncOmnibox(currentPageUrl);
                if (listener != null) listener.onPageStarted(currentPageUrl);
            }

            @Override
            public void onPageFinished(WebView view, String url) {
                currentPageUrl = url == null ? "" : url;
                syncOmnibox(currentPageUrl);
                if (listener != null) listener.onPageFinished(currentPageUrl);
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
    }

    private void submitOmnibox() {
        if (omnibox == null || webView == null) return;
        String resolved = BrowserAddressResolver.resolve(omnibox.getText().toString(), searchTemplate);
        omnibox.clearFocus();
        hidePanels();
        currentPageUrl = resolved;
        syncOmnibox(resolved);
        webView.loadUrl(resolved);
    }

    private void syncOmnibox(String url) {
        EditText field = omnibox;
        if (field == null || field.hasFocus()) return;
        String clean = url == null ? "" : url;
        if (!clean.equals(field.getText().toString())) field.setText(clean);
    }

    private void toggleTabsPanel() {
        if (tabsPanel == null) return;
        boolean show = tabsPanel.getVisibility() != View.VISIBLE;
        hidePanels();
        if (!show) return;
        if (tabsCurrent != null) {
            String label = currentPageUrl.isEmpty() ? "Current tab" : "Current: " + currentPageUrl;
            tabsCurrent.setText(label);
        }
        tabsPanel.setVisibility(View.VISIBLE);
        tabsPanel.bringToFront();
        if (bridgeStatus != null && bridgeStatus.getVisibility() == View.VISIBLE) bridgeStatus.bringToFront();
    }

    private void toggleMenuPanel() {
        if (menuPanel == null) return;
        boolean show = menuPanel.getVisibility() != View.VISIBLE;
        hidePanels();
        if (!show) return;
        menuPanel.setVisibility(View.VISIBLE);
        menuPanel.bringToFront();
        if (bridgeStatus != null && bridgeStatus.getVisibility() == View.VISIBLE) bridgeStatus.bringToFront();
    }

    private void hidePanels() {
        if (tabsPanel != null) tabsPanel.setVisibility(View.GONE);
        if (menuPanel != null) menuPanel.setVisibility(View.GONE);
    }

    void setBridgeStatus(String text) {
        String clean = text == null ? "" : text.trim();
        if (clean.equals(bridgeStatusText)) return;
        bridgeStatusText = clean;
        TextView status = bridgeStatus;
        if (status == null) return;
        if (clean.isEmpty()) {
            status.setText("");
            status.setVisibility(View.GONE);
        } else {
            status.setText(clean);
            status.setVisibility(View.VISIBLE);
            status.bringToFront();
        }
    }

    void loadUrl(String url) {
        if (webView != null) webView.loadUrl(url);
    }

    void setHomepage(String value) {
        homepage = BrowserAddressResolver.resolve(value, searchTemplate);
    }

    void setSearchTemplate(String value) {
        searchTemplate = value == null || value.trim().isEmpty()
                ? BrowserAddressResolver.DEFAULT_SEARCH_TEMPLATE : value.trim();
    }

    void focusOmnibox() {
        if (omnibox == null) return;
        omnibox.requestFocus();
        omnibox.selectAll();
    }

    boolean dispatchBrowserTouch(MotionEvent event) {
        return webView != null && event != null && webView.dispatchTouchEvent(event);
    }

    boolean goBackInBrowser() {
        if (webView == null || !webView.canGoBack()) return false;
        webView.goBack();
        return true;
    }

    boolean goForwardInBrowser() {
        if (webView == null || !webView.canGoForward()) return false;
        webView.goForward();
        return true;
    }

    boolean goHomeInBrowser() {
        if (webView == null) return false;
        hidePanels();
        webView.loadUrl(homepage);
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
        statusHandler.removeCallbacks(statusTick);
        WebView oldWebView = webView;
        webView = null;
        bridgeStatus = null;
        omnibox = null;
        tabsPanel = null;
        menuPanel = null;
        tabsCurrent = null;
        shellRoot = null;
        bridgeStatusText = "";
        if (oldWebView == null) return;
        // Do not navigate to about:blank during teardown: navigation callbacks are product state
        // and must not overwrite the last real URL that BrowserBridgeService will restore.
        oldWebView.stopLoading();
        oldWebView.removeJavascriptInterface("M4Input");
        oldWebView.setWebViewClient(new WebViewClient());
        oldWebView.setWebChromeClient(new WebChromeClient());
        oldWebView.clearHistory();
        ViewParentDetach.detach(oldWebView);
        oldWebView.removeAllViews();
        oldWebView.destroy();
    }

    /** Keeps WebView lifecycle cleanup explicit without exposing the view tree outside this class. */
    private static final class ViewParentDetach {
        static void detach(View view) {
            if (view == null || !(view.getParent() instanceof ViewGroup)) return;
            ((ViewGroup) view.getParent()).removeView(view);
        }
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
