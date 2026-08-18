package com.murphy.m4screenbridge;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.murphy.m4screenbridge.browser.BrowserBridgeService;

/** Control/status UI for both the M1 virtual browser FGS and the legacy accessibility bridge. */
public class MainActivity extends Activity {
    private TextView statusView;
    private TextView browserStatusView;
    private EditText browserUrlEt;
    private EditText m4HostEt;
    private EditText m4PortEt;
    private EditText thresholdEt;
    private EditText gapEt;
    private EditText delayEt;
    private CheckBox ditherCb;
    private CheckBox cacheCb;
    private RadioButton fitRb;
    private RadioButton coverRb;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            refresh();
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(buildView());
        load();
        ui.post(tick);
        handleLabIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleLabIntent(intent);
    }

    private void handleLabIntent(Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        if ("com.murphy.m4screenbridge.browser.SELF_TEST".equals(action)
                || "com.murphy.m4screenbridge.browser.LANDMARK".equals(action)
                || "com.murphy.m4screenbridge.browser.STOP".equals(action)) {
            if (intent.hasExtra(BrowserBridgeService.EXTRA_HOST)) {
                m4HostEt.setText(intent.getStringExtra(BrowserBridgeService.EXTRA_HOST));
            }
            if (intent.hasExtra(BrowserBridgeService.EXTRA_PORT)) {
                m4PortEt.setText(String.valueOf(intent.getIntExtra(BrowserBridgeService.EXTRA_PORT, Prefs.DEF_M4B3_PORT)));
            }
            if (intent.hasExtra(BrowserBridgeService.EXTRA_HOST)
                    || intent.hasExtra(BrowserBridgeService.EXTRA_PORT)) {
                saveM4Host();
            }
            if ("com.murphy.m4screenbridge.browser.STOP".equals(action)) {
                BrowserBridgeService.stop(this);
            } else if ("com.murphy.m4screenbridge.browser.LANDMARK".equals(action)) {
                BrowserBridgeService.startLandmarkTest(this);
            } else {
                BrowserBridgeService.startJavaScriptSelfTest(this);
            }
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        ScreenBridgeService.statusListener = new Runnable() {
            @Override
            public void run() {
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        refresh();
                    }
                });
            }
        };
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(tick);
        ScreenBridgeService.statusListener = null;
        // BrowserBridgeService intentionally outlives this Activity.
        super.onDestroy();
    }

    private View buildView() {
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(20);
        root.addView(title);

        statusView = new TextView(this);
        statusView.setText("服务未运行");
        statusView.setTextSize(14);
        root.addView(statusView);

        root.addView(label("虚拟浏览器 M1（独立前台服务 + dirty patch）"));
        browserStatusView = new TextView(this);
        browserStatusView.setText("虚拟浏览器 M1：前台服务未启动");
        browserStatusView.setTextSize(13);
        root.addView(browserStatusView);

        browserUrlEt = new EditText(this);
        browserUrlEt.setSingleLine(true);
        browserUrlEt.setHint("https://example.com/");
        browserUrlEt.setText("https://example.com/");
        root.addView(browserUrlEt);

        root.addView(label("M4 主机（空=loopback；实验室填阅读器 STA IP）"));
        m4HostEt = new EditText(this);
        m4HostEt.setSingleLine(true);
        m4HostEt.setHint("192.168.1.20 或 loopback");
        root.addView(m4HostEt);
        m4PortEt = new EditText(this);
        m4PortEt.setSingleLine(true);
        m4PortEt.setHint("48624");
        root.addView(m4PortEt);

        root.addView(button("启动 480×800 虚拟浏览器", new Runnable() {
            @Override
            public void run() {
                saveM4Host();
                BrowserBridgeService.startUrl(MainActivity.this, browserUrlEt.getText().toString());
                toast("已请求前台服务启动虚拟浏览器");
                ui.postDelayed(() -> refresh(), 300);
            }
        }));

        root.addView(button("启动 JavaScript 持续帧自测", new Runnable() {
            @Override
            public void run() {
                saveM4Host();
                BrowserBridgeService.startJavaScriptSelfTest(MainActivity.this);
                toast("自测页每秒翻转 40% 黑白区；关闭本界面或熄屏后服务应继续运行");
                ui.postDelayed(() -> refresh(), 300);
            }
        }));

        root.addView(button("启动非对称定位页 (TL/TR/BL/BR + A/B/C)", new Runnable() {
            @Override
            public void run() {
                saveM4Host();
                BrowserBridgeService.startLandmarkTest(MainActivity.this);
                toast("已启动非对称定位页，用于核对面板方向/镜像/极性");
                ui.postDelayed(() -> refresh(), 300);
            }
        }));

        root.addView(button("保存 M4 地址", new Runnable() {
            @Override
            public void run() {
                saveM4Host();
                toast("已保存 M4 主机；下次启动虚拟浏览器生效");
            }
        }));

        root.addView(button("注入 wrong-base NACK", new Runnable() {
            @Override
            public void run() {
                BrowserBridgeService.injectWrongBase(MainActivity.this);
                toast("已请求 wrong-base 注入");
            }
        }));

        root.addView(button("注入 CRC NACK", new Runnable() {
            @Override
            public void run() {
                BrowserBridgeService.injectCorruptCrc(MainActivity.this);
                toast("已请求 CRC 注入");
            }
        }));

        root.addView(button("停止虚拟浏览器前台服务", new Runnable() {
            @Override
            public void run() {
                BrowserBridgeService.stop(MainActivity.this);
                ui.postDelayed(() -> refresh(), 300);
            }
        }));

        root.addView(label("原有无障碍屏幕桥"));
        root.addView(button("打开无障碍设置", new Runnable() {
            @Override
            public void run() {
                startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS));
            }
        }));

        root.addView(button("开始会话", new Runnable() {
            @Override
            public void run() {
                save();
                if (isServiceEnabled()) {
                    if (ScreenBridgeService.startSession(5000)) {
                        toast("将在 5 秒后开始，请立即切回小说正文页");
                    } else {
                        toast("无障碍服务正在启动，请稍后再试");
                    }
                } else {
                    toast("请先在无障碍设置中启用“" + getString(R.string.service_label) + "”");
                    startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS));
                }
            }
        }));

        root.addView(button("停止会话", new Runnable() {
            @Override
            public void run() {
                save();
                ScreenBridgeService.stopSession();
                toast("会话已停止");
            }
        }));

        root.addView(label("黑白阈值（0–255，越高黑色越多）"));
        thresholdEt = new EditText(this);
        root.addView(thresholdEt);

        root.addView(label("自动排版最大行距（0–80）"));
        gapEt = new EditText(this);
        root.addView(gapEt);

        root.addView(label("手机翻页后预取等待（100–5000 毫秒）"));
        delayEt = new EditText(this);
        root.addView(delayEt);

        ditherCb = new CheckBox(this);
        ditherCb.setText("灰度抖动（图片较多时使用）");
        root.addView(ditherCb);

        cacheCb = new CheckBox(this);
        cacheCb.setText("启用前后页缓存（关闭后实时显示并转发触摸）");
        root.addView(cacheCb);

        RadioGroup crop = new RadioGroup(this);
        fitRb = new RadioButton(this);
        fitRb.setText("完整适应手机画面");
        coverRb = new RadioButton(this);
        coverRb.setText("自动排版正文（等比缩放，不压扁文字）");
        crop.addView(fitRb);
        crop.addView(coverRb);
        root.addView(crop);

        root.addView(button("应用并保存参数", new Runnable() {
            @Override
            public void run() {
                save();
                toast("参数已保存，下次截图起生效");
            }
        }));

        TextView help = new TextView(this);
        help.setText(R.string.instructions);
        help.setTextSize(13);
        root.addView(help);

        return scroll;
    }

    private boolean isServiceEnabled() {
        String flat = Settings.Secure.getString(getContentResolver(),
                Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
        if (flat == null) return false;
        ComponentName wanted = new ComponentName(this, ScreenBridgeService.class);
        for (String value : flat.split(":")) {
            ComponentName enabled = ComponentName.unflattenFromString(value);
            if (wanted.equals(enabled)) return true;
        }
        return false;
    }

    private void save() {
        SharedPreferences sp = getSharedPreferences(ScreenBridgeService.PREFS, Context.MODE_PRIVATE);
        SharedPreferences.Editor e = sp.edit();
        e.putInt(Prefs.KEY_THRESHOLD, intOf(thresholdEt, Prefs.DEF_THRESHOLD));
        e.putInt(Prefs.KEY_MAX_GAP, intOf(gapEt, Prefs.DEF_MAX_GAP));
        e.putInt(Prefs.KEY_PREFETCH_MS, intOf(delayEt, Prefs.DEF_PREFETCH_MS));
        e.putBoolean(Prefs.KEY_DITHER, ditherCb.isChecked());
        e.putString(Prefs.KEY_CROP, coverRb.isChecked() ? "cover" : "fit");
        e.putBoolean(Prefs.KEY_CACHE_ENABLED, cacheCb.isChecked());
        e.apply();
        saveM4Host();
        ScreenBridgeService.preferencesChanged();
    }

    private void saveM4Host() {
        SharedPreferences sp = getSharedPreferences(ScreenBridgeService.PREFS, Context.MODE_PRIVATE);
        SharedPreferences.Editor e = sp.edit();
        String host = m4HostEt.getText().toString().trim();
        e.putString(Prefs.KEY_M4B3_HOST, host);
        e.putInt(Prefs.KEY_M4B3_PORT, intOf(m4PortEt, Prefs.DEF_M4B3_PORT));
        e.apply();
    }

    private void load() {
        SharedPreferences sp = getSharedPreferences(ScreenBridgeService.PREFS, Context.MODE_PRIVATE);
        thresholdEt.setText(String.valueOf(sp.getInt(Prefs.KEY_THRESHOLD, Prefs.DEF_THRESHOLD)));
        gapEt.setText(String.valueOf(sp.getInt(Prefs.KEY_MAX_GAP, Prefs.DEF_MAX_GAP)));
        delayEt.setText(String.valueOf(sp.getInt(Prefs.KEY_PREFETCH_MS, Prefs.DEF_PREFETCH_MS)));
        ditherCb.setChecked(sp.getBoolean(Prefs.KEY_DITHER, false));
        cacheCb.setChecked(sp.getBoolean(Prefs.KEY_CACHE_ENABLED, true));
        boolean cover = "cover".equals(sp.getString(Prefs.KEY_CROP, "fit"));
        coverRb.setChecked(cover);
        fitRb.setChecked(!cover);
        m4HostEt.setText(sp.getString(Prefs.KEY_M4B3_HOST, ""));
        m4PortEt.setText(String.valueOf(sp.getInt(Prefs.KEY_M4B3_PORT, Prefs.DEF_M4B3_PORT)));
    }

    private void refresh() {
        StringBuilder sb = new StringBuilder();
        sb.append("无障碍服务：").append(isServiceEnabled() ? "已启用" : "未启用").append('\n');
        sb.append(ScreenBridgeService.snapshot());
        statusView.setText(sb.toString());
        if (browserStatusView != null) browserStatusView.setText(BrowserBridgeService.snapshot());
    }

    private Button button(String text, final Runnable onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                onClick.run();
            }
        });
        return b;
    }

    private TextView label(String t) {
        TextView tv = new TextView(this);
        tv.setText(t);
        tv.setPadding(0, dp(8), 0, 0);
        return tv;
    }

    private void toast(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private static int intOf(EditText et, int def) {
        try {
            return Integer.parseInt(et.getText().toString().trim());
        } catch (NumberFormatException ex) {
            return def;
        }
    }
}
