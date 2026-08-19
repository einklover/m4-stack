package com.murphy.m4screenbridge.browser.shell;

import android.content.Context;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Fixed-geometry, app-owned keyboard rendered inside the M4 browser Presentation. */
public final class M4KeyboardView extends LinearLayout {
    public interface Listener {
        void onTextChanged(String text);
        void onSubmit(String text);
        void onHideRequested();
    }

    private final BrowserKeyboardState state = new BrowserKeyboardState();
    private final Listener listener;
    private boolean replaceOnNextTextKey;

    public M4KeyboardView(Context context, Listener listener) {
        super(context);
        if (listener == null) throw new IllegalArgumentException("listener is null");
        this.listener = listener;
        setOrientation(VERTICAL);
        setBackgroundColor(BrowserShellStyle.BLACK);
        setVisibility(View.GONE);
    }

    public void showForText(String initialText) {
        state.replace(initialText);
        replaceOnNextTextKey = true;
        rebuild();
        setVisibility(View.VISIBLE);
        bringToFront();
    }

    public void hideKeyboard() {
        setVisibility(View.GONE);
        replaceOnNextTextKey = false;
    }

    public boolean isShowing() {
        return getVisibility() == View.VISIBLE;
    }

    public String text() {
        return state.text();
    }

    private void rebuild() {
        removeAllViews();
        if (state.mode() == BrowserKeyboardState.Mode.LETTERS) {
            addLetterRows();
        } else {
            addSymbolRows();
        }
    }

    private void addLetterRows() {
        addTextRow(new String[] {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"});
        addTextRow(new String[] {"a", "s", "d", "f", "g", "h", "j", "k", "l"});

        LinearLayout row3 = newRow();
        row3.addView(key("⇧", () -> {
            state.toggleShift();
            rebuild();
        }), weightedKey(1.2f));
        for (String key : new String[] {"z", "x", "c", "v", "b", "n", "m"}) {
            row3.addView(key(labelForLetter(key), () -> appendKey(key)), weightedKey(1f));
        }
        row3.addView(key("⌫", this::backspace), weightedKey(1.2f));
        addView(row3, rowParams());

        LinearLayout row4 = newRow();
        row4.addView(key("123", () -> {
            state.toggleMode();
            rebuild();
        }), weightedKey(1.2f));
        row4.addView(key("Space", this::space), weightedKey(2.3f));
        row4.addView(key("Go", () -> listener.onSubmit(state.text())), weightedKey(1.2f));
        row4.addView(key("Hide", listener::onHideRequested), weightedKey(1.3f));
        addView(row4, rowParams());
    }

    private void addSymbolRows() {
        addTextRow(new String[] {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"});
        addTextRow(new String[] {".", "/", ":", "-", "_", "?", "&", "=", "%", "+"});

        LinearLayout row3 = newRow();
        for (String key : new String[] {"@", "#", "$", "*", "(", ")", "!", "'", "\""}) {
            row3.addView(key(key, () -> appendKey(key)), weightedKey(1f));
        }
        row3.addView(key("⌫", this::backspace), weightedKey(1.2f));
        addView(row3, rowParams());

        LinearLayout row4 = newRow();
        row4.addView(key("ABC", () -> {
            state.toggleMode();
            rebuild();
        }), weightedKey(1.2f));
        row4.addView(key("Space", this::space), weightedKey(2.3f));
        row4.addView(key("Go", () -> listener.onSubmit(state.text())), weightedKey(1.2f));
        row4.addView(key("Hide", listener::onHideRequested), weightedKey(1.3f));
        addView(row4, rowParams());
    }

    private void addTextRow(String[] keys) {
        LinearLayout row = newRow();
        for (String key : keys) {
            row.addView(key(labelForLetter(key), () -> appendKey(key)), weightedKey(1f));
        }
        addView(row, rowParams());
    }

    private String labelForLetter(String key) {
        if (state.mode() != BrowserKeyboardState.Mode.LETTERS || !state.shifted()) return key;
        return key.toUpperCase(java.util.Locale.ROOT);
    }

    private void appendKey(String value) {
        prepareForTextMutation();
        state.append(value);
        listener.onTextChanged(state.text());
        if (state.mode() == BrowserKeyboardState.Mode.LETTERS) rebuild();
    }

    private void space() {
        prepareForTextMutation();
        state.space();
        listener.onTextChanged(state.text());
    }

    private void backspace() {
        if (replaceOnNextTextKey) {
            state.clear();
            replaceOnNextTextKey = false;
        } else {
            state.backspace();
        }
        listener.onTextChanged(state.text());
    }

    private void prepareForTextMutation() {
        if (!replaceOnNextTextKey) return;
        state.clear();
        replaceOnNextTextKey = false;
    }

    private LinearLayout newRow() {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        row.setBackgroundColor(BrowserShellStyle.BLACK);
        return row;
    }

    private LinearLayout.LayoutParams rowParams() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, BrowserShellStyle.TOUCH_MIN);
    }

    private LinearLayout.LayoutParams weightedKey(float weight) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, weight);
        lp.setMargins(1, 1, 1, 1);
        return lp;
    }

    private TextView key(String label, Runnable action) {
        TextView view = new TextView(getContext());
        view.setText(label);
        view.setTextColor(BrowserShellStyle.BLACK);
        view.setBackgroundColor(BrowserShellStyle.WHITE);
        view.setTextSize(15);
        view.setGravity(Gravity.CENTER);
        view.setClickable(true);
        // Keyboard keys must not steal focus from the omnibox. Keeping focus on the editor prevents
        // page callbacks from overwriting in-progress text while the user taps the app-owned keys.
        view.setFocusable(false);
        view.setOnClickListener(v -> action.run());
        return view;
    }
}
