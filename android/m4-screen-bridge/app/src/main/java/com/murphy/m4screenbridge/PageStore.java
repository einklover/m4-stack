package com.murphy.m4screenbridge;

import java.util.concurrent.ConcurrentHashMap;

/** Bounded page cache. Retains at least 8 pages behind the consumed index. */
public final class PageStore {
    private static final int KEEP_BACK = 8;

    private final ConcurrentHashMap<Integer, byte[]> pages = new ConcurrentHashMap<>();
    private volatile int consumedIndex = 0;

    public void put(int index, byte[] raw) {
        pages.put(index, raw);
        prune();
    }

    public byte[] get(int index) {
        return pages.get(index);
    }

    public boolean contains(int index) {
        return pages.containsKey(index);
    }

    public int consumedIndex() {
        return consumedIndex;
    }

    public int lo() {
        int m = Integer.MAX_VALUE;
        for (int k : pages.keySet()) m = Math.min(m, k);
        return m == Integer.MAX_VALUE ? -1 : m;
    }

    public int hi() {
        int m = -1;
        for (int k : pages.keySet()) m = Math.max(m, k);
        return m;
    }

    public int count() {
        return pages.size();
    }

    public void reset() {
        pages.clear();
        consumedIndex = 0;
    }

    public void consume(int index) {
        if (index > consumedIndex) consumedIndex = index;
        prune();
    }

    private void prune() {
        int lo = consumedIndex - KEEP_BACK;
        for (int k : pages.keySet()) {
            if (k < lo) pages.remove(k);
        }
    }
}
