package com.murphy.m4screenbridge;

/** Pure-Java parser for Xiaohongshu accessibility card descriptions. */
public final class XhsFeedParser {
    private XhsFeedParser() { }

    public static final class Item {
        public final String title;
        public final String author;
        public final String likes;

        Item(String title, String author, String likes) {
            this.title = title;
            this.author = author;
            this.likes = likes;
        }
    }

    /** Accept text/image notes only. Video and live cards deliberately fail closed. */
    public static Item parse(String description) {
        if (description == null) return null;
        String value = description.trim();
        if (!value.startsWith("笔记")) return null;

        value = value.substring(2).trim();
        int from = value.lastIndexOf(" 来自");
        if (from <= 0) return null;
        String title = value.substring(0, from).trim();
        String tail = value.substring(from + 3).trim();
        if (title.isEmpty() || tail.isEmpty()) return null;

        String author = tail;
        String likes = "";
        int likeSuffix = tail.lastIndexOf('赞');
        if (likeSuffix == tail.length() - 1) {
            int space = tail.lastIndexOf(' ', likeSuffix);
            if (space > 0) {
                String candidate = tail.substring(space + 1, likeSuffix).trim();
                if (isCount(candidate)) {
                    author = tail.substring(0, space).trim();
                    likes = candidate;
                }
            }
        }
        return author.isEmpty() ? null : new Item(title, author, likes);
    }

    /** Stable across accessibility-tree revisions; collisions are resolved by the cache. */
    public static String stableToken(String title, String author) {
        String value = (title == null ? "" : title) + '\n' + (author == null ? "" : author);
        return "n" + Integer.toUnsignedString(value.hashCode(), 36);
    }

    private static boolean isCount(String value) {
        if (value.isEmpty()) return false;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if ((c < '0' || c > '9') && c != '.' && c != '万' && c != '+') return false;
        }
        return true;
    }
}
