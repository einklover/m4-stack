package com.murphy.m4screenbridge.browser;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Restores an explicitly enabled product Browser Bridge session after device/package restart. */
public final class BrowserBridgeStartupReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (context == null || intent == null) return;
        String action = intent.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) {
            BrowserBridgeService.resumeIfConfigured(context);
        }
    }
}
