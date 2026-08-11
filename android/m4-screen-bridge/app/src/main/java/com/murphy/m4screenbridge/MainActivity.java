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
import android.widget.TextView;
import android.widget.Toast;

/** Simple UI: accessibility shortcut, session control, tunables, live status. */
public class MainActivity extends Activity {
    private TextView statusView;
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
        super.onDestroy();
    }

    private LinearLayout buildView() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextSize(20);
        root.addView(title);

        statusView = new TextView(this);
        statusView.setText("服务未运行");
        statusView.setTextSize(14);
        root.addView(statusView);

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

        return root;
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
        ScreenBridgeService.preferencesChanged();
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
    }

    private void refresh() {
        StringBuilder sb = new StringBuilder();
        sb.append("无障碍服务：").append(isServiceEnabled() ? "已启用" : "未启用").append('\n');
        sb.append(ScreenBridgeService.snapshot());
        statusView.setText(sb.toString());
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
