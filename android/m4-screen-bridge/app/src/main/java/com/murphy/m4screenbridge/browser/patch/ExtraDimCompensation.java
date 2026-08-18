package com.murphy.m4screenbridge.browser.patch;

/**
 * Inverts Android Extra Dim / reduce-bright-colors so the logical MONO1
 * threshold sees content colors, not the phone-panel accessibility dim.
 *
 * Extra Dim is a uniform RGB scale {@code k = a*s^2 + b*s + c} with
 * {@code s = strength/100}. Motorola / AOSP linear defaults reproduce the
 * observed XT2437-4 capture: slider 99 → strength 89 → k≈0.1496 →
 * CSS #ffffff arrives as rgba(38,37,35,255).
 */
public final class ExtraDimCompensation {
    public static final int UNITY_GAIN = 256;
    public static final String SETTING_ACTIVATED = "reduce_bright_colors_activated";
    public static final String SETTING_LEVEL = "reduce_bright_colors_level";

    /** AOSP/Motorola {@code config_reduceBrightColorsCoefficients}. */
    public static final float DEFAULT_A = 0f;
    public static final float DEFAULT_B = -0.955555555555554f;
    public static final float DEFAULT_C = 1f;
    public static final int DEFAULT_STRENGTH_MIN = 25;
    public static final int DEFAULT_STRENGTH_MAX = 90;

    private ExtraDimCompensation() {}

    public static int strengthFromSlider(int slider0to100, int min, int max) {
        if (min < 0) min = 0;
        if (max > 100) max = 100;
        if (max < min) max = min;
        if (slider0to100 <= 0) return min;
        if (slider0to100 >= 100) return max;
        return min + (max - min) * slider0to100 / 100;
    }

    public static float componentValue(int strength, float a, float b, float c) {
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        float s = strength / 100f;
        float k = (a * s * s) + (b * s) + c;
        if (k < 0.05f) return 0.05f;
        if (k > 1f) return 1f;
        return k;
    }

    public static int inverseGain256(boolean activated, int slider0to100) {
        return inverseGain256(activated, slider0to100,
                DEFAULT_A, DEFAULT_B, DEFAULT_C,
                DEFAULT_STRENGTH_MIN, DEFAULT_STRENGTH_MAX);
    }

    public static int inverseGain256(boolean activated, int slider0to100,
            float a, float b, float c, int min, int max) {
        if (!activated) return UNITY_GAIN;
        int strength = strengthFromSlider(slider0to100, min, max);
        float k = componentValue(strength, a, b, c);
        return Math.round(UNITY_GAIN / k);
    }

    public static int applyGain(int channel, int gain256) {
        if (gain256 == UNITY_GAIN) return channel;
        int scaled = (channel * gain256) / UNITY_GAIN;
        if (scaled < 0) return 0;
        if (scaled > 255) return 255;
        return scaled;
    }

    /**
     * If the brightest captured luma is still below the MONO1 threshold, no pixel
     * can become white. Scale so {@code lumaMax} maps to 255. This recovers Extra
     * Dim / reduce-bright-colors without reading @hide Settings keys.
     */
    public static int autoGain256(int lumaMax, int threshold) {
        if (lumaMax <= 0 || lumaMax >= threshold) return UNITY_GAIN;
        return Math.round(255f * UNITY_GAIN / (float) lumaMax);
    }
}
