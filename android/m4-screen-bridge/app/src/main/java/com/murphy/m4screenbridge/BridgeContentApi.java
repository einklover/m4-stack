package com.murphy.m4screenbridge;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Handler;
import android.view.accessibility.AccessibilityNodeInfo;

import java.io.ByteArrayOutputStream;
import java.net.URLDecoder;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** Structured phone-app and autonomous Xiaohongshu reading API served to M4. */
public final class BridgeContentApi {
    private static final String XHS_PACKAGE = "com.xingin.xhs";
    private static final int FEED_TARGET = 24;
    private static final int COMMENT_TARGET = 18;

    private enum Page { OUTSIDE, HOME, NOTE, COMMENTS, OTHER }

    private static final class FeedEntry {
        String token;
        String description;
        String title;
        String author;
        String likes;
        Rect bounds;
        Note note = new Note();
        ArrayList<Comment> comments = new ArrayList<>();
        ArrayList<byte[]> images = new ArrayList<>();
    }

    private static final class Note {
        String title = "";
        String author = "";
        String body = "";
        String commentCount = "";
        String likes = "";
        String collects = "";
        String timestamp = "";
        int imageCount = 0;
        int imageIndex = 0;
    }

    private static final class Comment {
        String author = "";
        String body = "";
        String meta = "";
    }

    private final ScreenBridgeService service;
    private final Handler main;
    private final Object automationLock = new Object();
    private final Object dataLock = new Object();
    private final LinkedHashMap<String, FeedEntry> feed = new LinkedHashMap<>();
    private final ArrayList<Comment> comments = new ArrayList<>();
    private final ExecutorService automation = Executors.newSingleThreadExecutor();
    private final AtomicBoolean feedCollecting = new AtomicBoolean(false);
    private final AtomicBoolean commentCollecting = new AtomicBoolean(false);
    private volatile boolean readerRequested;
    private FeedEntry currentEntry;
    private Note currentNote = new Note();

    BridgeContentApi(ScreenBridgeService service, Handler main) {
        this.service = service;
        this.main = main;
    }

    void shutdown() { automation.shutdownNow(); }

    public String appsJson() {
        PackageManager pm = service.getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> resolved = pm.queryIntentActivities(launcher, 0);
        resolved.sort(Comparator.comparing(r -> r.loadLabel(pm).toString(), String.CASE_INSENSITIVE_ORDER));
        StringBuilder out = new StringBuilder("{\"ok\":true,\"apps\":[");
        Set<String> seen = new HashSet<>();
        boolean comma = false;
        for (ResolveInfo r : resolved) {
            if (r.activityInfo == null) continue;
            String pkg = r.activityInfo.packageName;
            if (pkg == null || pkg.equals(service.getPackageName()) || !seen.add(pkg)) continue;
            if (comma) out.append(',');
            comma = true;
            out.append("{\"id\":\"").append(json(pkg)).append("\",\"title\":\"")
                    .append(json(r.loadLabel(pm).toString())).append("\",\"mode\":\"")
                    .append(XHS_PACKAGE.equals(pkg) ? "content" : "mirror")
                    .append("\",\"subtitle\":\"")
                    .append(XHS_PACKAGE.equals(pkg) ? "离线式图文阅读 · 正文/图片/评论" : "屏幕镜像")
                    .append("\"}");
        }
        return out.append("]}").toString();
    }

    public boolean openApp(String encodedPackage) {
        String pkg = decode(encodedPackage);
        Intent launch = service.getPackageManager().getLaunchIntentForPackage(pkg);
        if (launch == null) return false;
        launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        service.startActivity(launch);
        if (XHS_PACKAGE.equals(pkg)) triggerFeedCollection();
        return true;
    }

    public String xhsFeedJson() {
        readerRequested = false;
        triggerFeedCollection();
        Page page = callOnMain(this::detectPage, Page.OUTSIDE);
        List<FeedEntry> snapshot;
        synchronized (dataLock) {
            snapshot = new ArrayList<>();
            for (FeedEntry e : feed.values()) if (!e.note.body.isEmpty()) snapshot.add(e);
        }
        StringBuilder out = new StringBuilder("{\"ok\":true,\"kind\":\"text_notes\",\"state\":\"")
                .append(pageName(page)).append("\",\"activity\":\"")
                .append(json(activityName(page))).append("\",\"collecting\":")
                .append(feedCollecting.get()).append(",\"cached\":").append(snapshot.size())
                .append(",\"discovered\":").append(feedSize()).append(",\"target\":")
                .append(FEED_TARGET).append(",\"items\":[");
        for (int i = 0; i < snapshot.size(); i++) {
            FeedEntry e = snapshot.get(i);
            if (i != 0) out.append(',');
            out.append("{\"token\":\"").append(e.token)
                    .append("\",\"title\":\"").append(json(e.title))
                    .append("\",\"author\":\"").append(json(e.author))
                    .append("\",\"likes\":\"").append(json(e.likes)).append("\"}");
        }
        return out.append("]}").toString();
    }

    public boolean openXhsNote(String encodedToken) {
        readerRequested = true;
        String token = decode(encodedToken);
        FeedEntry target;
        synchronized (dataLock) { target = feed.get(token); }
        if (target == null) return false;
        synchronized (dataLock) {
            if (!target.note.body.isEmpty()) {
                currentEntry = target;
                currentNote = copyNote(target.note);
                comments.clear();
                comments.addAll(target.comments);
                return true;
            }
        }
        synchronized (automationLock) {
            if (!ensureHome()) return false;
            if (callOnMain(() -> clickVisibleFeedEntry(target), false)) return finishOpeningNote(target);
            // Cached cards may be above or below the phone's background crawl
            // position. Rewind first, then scan forward so every returned token
            // remains actionable after later prefetch passes.
            for (int pass = 0; pass < 12; pass++) {
                if (!callOnMain(this::scrollFeedBackward, false)) break;
                pause(260);
                if (callOnMain(() -> clickVisibleFeedEntry(target), false)) return finishOpeningNote(target);
            }
            for (int pass = 0; pass < 22; pass++) {
                if (callOnMain(() -> clickVisibleFeedEntry(target), false)) {
                    return finishOpeningNote(target);
                }
                if (!callOnMain(this::scrollFeedForward, false)) break;
                pause(280);
            }
        }
        return false;
    }

    private boolean finishOpeningNote(FeedEntry target) {
        if (!waitForPage(Page.NOTE, 3500)) return false;
        currentEntry = target;
        synchronized (dataLock) { comments.clear(); currentNote = new Note(); }
        readNoteWithScroll();
        return true;
    }

    public String xhsNoteJson() {
        boolean needsRead;
        synchronized (dataLock) { needsRead = currentNote.body.isEmpty(); }
        if (needsRead && ensureCurrentNoteShallow()) {
            synchronized (automationLock) { readNoteWithScroll(); }
        }
        Note note;
        synchronized (dataLock) { note = copyNote(currentNote); }
        FeedEntry fallback = currentEntry;
        if (note.title.isEmpty() && fallback != null) note.title = fallback.title;
        if (note.author.isEmpty() && fallback != null) note.author = fallback.author;
        return "{\"ok\":true,\"state\":\"" + pageName(callOnMain(this::detectPage, Page.OUTSIDE))
                + "\",\"readable\":" + (!note.title.isEmpty() && !note.body.isEmpty())
                + ",\"title\":\"" + json(note.title)
                + "\",\"author\":\"" + json(note.author)
                + "\",\"body\":\"" + json(note.body)
                + "\",\"imageCount\":" + note.imageCount
                + ",\"imageIndex\":" + note.imageIndex
                + ",\"likes\":\"" + json(note.likes)
                + "\",\"collects\":\"" + json(note.collects)
                + "\",\"timestamp\":\"" + json(note.timestamp)
                + "\",\"commentCount\":\"" + json(note.commentCount) + "\"}";
    }

    public boolean openXhsComments() {
        readerRequested = true;
        triggerCommentCollection(COMMENT_TARGET);
        return currentEntry != null;
    }

    public String xhsCommentsJson(boolean advance) {
        if (advance) triggerCommentCollection(COMMENT_TARGET + 16);
        List<Comment> snapshot;
        synchronized (dataLock) { snapshot = new ArrayList<>(comments); }
        StringBuilder out = new StringBuilder("{\"ok\":true,\"collecting\":")
                .append(commentCollecting.get()).append(",\"cached\":").append(snapshot.size())
                .append(",\"items\":[");
        for (int i = 0; i < snapshot.size(); i++) {
            Comment c = snapshot.get(i);
            if (i != 0) out.append(',');
            out.append("{\"author\":\"").append(json(c.author))
                    .append("\",\"body\":\"").append(json(c.body))
                    .append("\",\"meta\":\"").append(json(c.meta)).append("\"}");
        }
        return out.append("]}").toString();
    }

    public byte[] xhsImageBmp(int requestedIndex) {
        FeedEntry selected = currentEntry;
        if (selected != null) {
            synchronized (dataLock) {
                if (requestedIndex >= 0 && requestedIndex < selected.images.size()) {
                    byte[] cached = selected.images.get(requestedIndex);
                    if (cached != null) return cached;
                }
            }
        }
        synchronized (automationLock) {
            if (currentEntry == null || !ensureCurrentNote()) return null;
            int count;
            synchronized (dataLock) { count = currentNote.imageCount; }
            if (count <= 0) return null;
            int target = Math.max(0, Math.min(count - 1, requestedIndex));
            for (int i = 0; i < 14 && !callOnMain(this::imageVisible, false); i++) {
                service.swipeScreen(540, 500, 540, 2050, 360);
                pause(350);
            }
            for (int i = 0; i < count + 1; i++) {
                int current = callOnMain(this::visibleImageIndex, 0);
                if (current == target) break;
                final boolean forward = current < target;
                if (!callOnMain(() -> scrollImage(forward), false)) break;
                pause(420);
            }
            byte[] bmp = captureVisibleImageBmp();
            if (bmp != null) synchronized (dataLock) {
                currentNote.imageIndex = target;
                while (selected.images.size() <= target) selected.images.add(null);
                selected.images.set(target, bmp);
            }
            return bmp;
        }
    }

    private void triggerFeedCollection() {
        if (!feedCollecting.compareAndSet(false, true)) return;
        automation.execute(() -> {
            try {
                pause(900);
                synchronized (automationLock) {
                    if (!ensureHome()) return;
                    int stale = 0;
                    for (int pass = 0; pass < 40; pass++) {
                        if (readerRequested) break;
                        List<FeedEntry> visible = callOnMain(this::readVisibleFeed, new ArrayList<>());
                        int added = mergeFeed(visible);
                        stale = added == 0 ? stale + 1 : 0;
                        boolean prefetched = false;
                        for (FeedEntry candidate : visible) {
                            FeedEntry stored;
                            synchronized (dataLock) { stored = feed.get(candidate.token); }
                            if (stored != null && stored.note.body.isEmpty() && prefetchEntry(stored)) {
                                prefetched = true;
                                break;
                            }
                        }
                        if (readerRequested || readyFeedCount() >= FEED_TARGET) break;
                        // Returning from a note preserves the current feed viewport. Process
                        // every visible card before advancing, otherwise Xiaohongshu recycles
                        // the off-screen nodes and the skipped cards can no longer be opened.
                        if (prefetched) continue;
                        if (stale >= 3 || !callOnMain(this::scrollFeedForward, false)) break;
                        pause(520);
                    }
                }
            } finally {
                feedCollecting.set(false);
            }
        });
    }

    private boolean prefetchEntry(FeedEntry target) {
        if (!callOnMain(() -> clickVisibleFeedEntry(target), false)) return false;
        if (!waitForPage(Page.NOTE, 3000)) return false;
        // Capture the carousel before scrolling to body/comments. Once the
        // detail RecyclerView is deep in comments, XHS may recycle image nodes.
        Note initial = callOnMain(this::readVisibleNote, new Note());
        List<byte[]> visibleImages = cacheVisibleImages(initial.imageCount);
        Note snapshot = readNoteSnapshotWithScroll();
        mergeNoteInto(snapshot, initial);
        List<Comment> visibleComments = readCommentsSnapshot(6, 4);
        synchronized (dataLock) {
            target.note = snapshot;
            target.comments.clear();
            target.comments.addAll(visibleComments);
            target.images.clear();
            target.images.addAll(visibleImages);
        }
        service.globalBack();
        waitForPage(Page.HOME, 2500);
        pause(300);
        return !target.note.body.isEmpty();
    }

    private void triggerCommentCollection(int target) {
        if (!commentCollecting.compareAndSet(false, true)) return;
        automation.execute(() -> {
            try {
                synchronized (automationLock) {
                    if (!ensureCurrentNote()) return;
                    int stale = 0;
                    for (int pass = 0; pass < 12; pass++) {
                        int added = mergeComments(callOnMain(this::readVisibleComments, new ArrayList<>()));
                        stale = added == 0 ? stale + 1 : 0;
                        synchronized (dataLock) { if (comments.size() >= target) break; }
                        if (stale >= 3 || !scrollNoteForward()) break;
                        pause(500);
                    }
                }
            } finally {
                commentCollecting.set(false);
            }
        });
    }

    private boolean ensureHome() {
        if (!XHS_PACKAGE.equals(service.foregroundPackageSnapshot())) {
            Intent launch = service.getPackageManager().getLaunchIntentForPackage(XHS_PACKAGE);
            if (launch == null) return false;
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
            service.startActivity(launch);
            pause(1200);
        }
        for (int i = 0; i < 5; i++) {
            Page page = callOnMain(this::detectPage, Page.OUTSIDE);
            if (page == Page.HOME) {
                if (callOnMain(this::recommendationSelected, false)) return true;
                callOnMain(() -> clickByDescription("发现"), false);
                pause(250);
                callOnMain(() -> clickByText("推荐"), false);
                pause(350);
                return true;
            }
            if (page == Page.NOTE || page == Page.COMMENTS) service.globalBack();
            else callOnMain(() -> clickByIdSuffix("/index_home") || clickByDescription("首页"), false);
            pause(650);
        }
        return callOnMain(this::detectPage, Page.OUTSIDE) == Page.HOME;
    }

    private boolean ensureCurrentNote() {
        Page p = callOnMain(this::detectPage, Page.OUTSIDE);
        if ((p == Page.NOTE || p == Page.COMMENTS) && currentEntry != null) return true;
        FeedEntry entry = currentEntry;
        if (entry == null || !ensureHome()) return false;
        if (callOnMain(() -> clickVisibleFeedEntry(entry), false)) return waitForPage(Page.NOTE, 3500);
        for (int pass = 0; pass < 12; pass++) {
            if (!callOnMain(this::scrollFeedBackward, false)) break;
            pause(260);
            if (callOnMain(() -> clickVisibleFeedEntry(entry), false)) return waitForPage(Page.NOTE, 3500);
        }
        for (int pass = 0; pass < 22; pass++) {
            if (callOnMain(() -> clickVisibleFeedEntry(entry), false)) return waitForPage(Page.NOTE, 3500);
            if (!callOnMain(this::scrollFeedForward, false)) break;
            pause(280);
        }
        return false;
    }

    private void readNoteWithScroll() {
        if (currentEntry == null || !ensureCurrentNoteShallow()) return;
        Note snapshot = readNoteSnapshotWithScroll();
        synchronized (dataLock) { mergeNoteInto(currentNote, snapshot); }
    }

    private Note readNoteSnapshotWithScroll() {
        Note result = new Note();
        for (int i = 0; i < 5; i++) {
            Note visible = callOnMain(this::readVisibleNote, new Note());
            mergeNoteInto(result, visible);
            if (!result.body.isEmpty()) break;
            if (!scrollNoteForward()) break;
            pause(420);
        }
        return result;
    }

    private boolean ensureCurrentNoteShallow() {
        Page p = callOnMain(this::detectPage, Page.OUTSIDE);
        return p == Page.NOTE || p == Page.COMMENTS;
    }

    private Page detectPage() {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null || !XHS_PACKAGE.equals(String.valueOf(root.getPackageName()))) return Page.OUTSIDE;
        try {
            // Accessibility services receive a pruned tree, while uiautomator
            // sees every view. Use several stable semantic anchors instead of
            // depending on the non-important noteDetailRoot container.
            if (hasId(root, "/noteDetailRoot") || hasId(root, "/noteContent")
                    || hasId(root, "/imageListView") || hasId(root, "/imageNoteTextView")) {
                return hasId(root, "/tv_content") ? Page.COMMENTS : Page.NOTE;
            }
            if (hasId(root, "/mLoadMoreRecycleView")) return Page.HOME;
            return Page.OTHER;
        } finally { root.recycle(); }
    }

    private boolean recommendationSelected() {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return false;
        try { return hasDescription(root, "已选定推荐"); } finally { root.recycle(); }
    }

    private List<FeedEntry> readVisibleFeed() {
        ArrayList<FeedEntry> out = new ArrayList<>();
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return out;
        try { collectFeed(root, out, new HashSet<>()); } finally { root.recycle(); }
        return out;
    }

    private void collectFeed(AccessibilityNodeInfo node, List<FeedEntry> out, Set<String> seen) {
        String description = descriptionOf(node);
        XhsFeedParser.Item parsed = XhsFeedParser.parse(description);
        if (parsed != null && seen.add(description)) {
            FeedEntry e = new FeedEntry();
            e.token = XhsFeedParser.stableToken(parsed.title, parsed.author);
            e.description = description;
            e.title = parsed.title;
            e.author = parsed.author;
            e.likes = parsed.likes;
            e.bounds = new Rect();
            node.getBoundsInScreen(e.bounds);
            out.add(e);
        }
        forEachChild(node, child -> collectFeed(child, out, seen));
    }

    private int mergeFeed(List<FeedEntry> visible) {
        int added = 0;
        synchronized (dataLock) {
            for (FeedEntry e : visible) {
                String token = e.token;
                int suffix = 1;
                while (feed.containsKey(token) && !sameFeed(feed.get(token), e)) token = e.token + "-" + suffix++;
                e.token = token;
                if (!feed.containsKey(token)) { feed.put(token, e); added++; }
                else {
                    FeedEntry old = feed.get(token);
                    old.description = e.description;
                    old.bounds = e.bounds;
                    old.likes = e.likes;
                }
            }
            while (feed.size() > 60) feed.remove(feed.keySet().iterator().next());
        }
        return added;
    }

    private int feedSize() { synchronized (dataLock) { return feed.size(); } }

    private int readyFeedCount() {
        synchronized (dataLock) {
            int count = 0;
            for (FeedEntry e : feed.values()) if (!e.note.body.isEmpty()) count++;
            return count;
        }
    }

    private boolean clickVisibleFeedEntry(FeedEntry target) {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return false;
        try {
            AccessibilityNodeInfo card = findByDescription(root, target.description);
            if (card == null) return false;
            Rect bounds = new Rect();
            card.getBoundsInScreen(bounds);
            // The descriptive card node changes between XHS releases: some
            // builds make a child clickable, others make only its parent row
            // clickable. Current builds expose neither, so the card bounds are
            // the final stable signal and accessibility injects a center tap.
            if (clickNode(card)) return true;
            return bounds.width() > 0 && bounds.height() > 0
                    && service.tapScreen(bounds.exactCenterX(), bounds.exactCenterY());
        } finally { root.recycle(); }
    }

    private boolean scrollFeedForward() { return actionOnId("/mLoadMoreRecycleView", AccessibilityNodeInfo.ACTION_SCROLL_FORWARD); }
    private boolean scrollFeedBackward() { return actionOnId("/mLoadMoreRecycleView", AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD); }

    private boolean scrollNoteForward() {
        boolean action = callOnMain(() -> actionOnId("/noteDetailRV", AccessibilityNodeInfo.ACTION_SCROLL_FORWARD), false);
        if (!action) action = service.swipeScreen(540, 1900, 540, 520, 380);
        return action;
    }

    private Note readVisibleNote() {
        Note note = new Note();
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return note;
        try { collectNote(root, note); } finally { root.recycle(); }
        return note;
    }

    private void collectNote(AccessibilityNodeInfo node, Note note) {
        String id = idOf(node);
        String value = textOf(node);
        String description = descriptionOf(node);
        if (id.endsWith("/noteTitleTV") && !value.isEmpty()) note.title = value;
        else if (id.endsWith("/nickNameTV") && !value.isEmpty()) note.author = value;
        else if (id.endsWith("/imageNoteTextView") && !value.isEmpty()) note.body = value;
        else if (id.endsWith("/noteCommentCountTV") || id.endsWith("/noteCommentTV")) note.commentCount = digits(value);
        else if (id.endsWith("/noteLikeTV")) note.likes = digits(value);
        else if (id.endsWith("/noteCollectTV")) note.collects = digits(value);
        else if (id.endsWith("/noteTimestamp")) note.timestamp = description;
        int[] image = parseImagePosition(description);
        if (image[1] > 0) { note.imageIndex = image[0]; note.imageCount = image[1]; }
        forEachChild(node, child -> collectNote(child, note));
    }

    private static void mergeNoteInto(Note target, Note incoming) {
        if (!incoming.title.isEmpty()) target.title = incoming.title;
        if (!incoming.author.isEmpty()) target.author = incoming.author;
        if (!incoming.body.isEmpty()) target.body = incoming.body;
        if (!incoming.commentCount.isEmpty()) target.commentCount = incoming.commentCount;
        if (!incoming.likes.isEmpty()) target.likes = incoming.likes;
        if (!incoming.collects.isEmpty()) target.collects = incoming.collects;
        if (!incoming.timestamp.isEmpty()) target.timestamp = incoming.timestamp;
        if (incoming.imageCount > 0) { target.imageCount = incoming.imageCount; target.imageIndex = incoming.imageIndex; }
    }

    private List<Comment> readVisibleComments() {
        ArrayList<String> authors = new ArrayList<>(), bodies = new ArrayList<>(), metas = new ArrayList<>();
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return new ArrayList<>();
        try {
            collectValues(root, "/tv_user_name", authors);
            collectValues(root, "/tv_content", bodies);
            collectValues(root, "/newTimePoiIpTransTv", metas);
        } finally { root.recycle(); }
        ArrayList<Comment> out = new ArrayList<>();
        for (int i = 0; i < Math.min(authors.size(), bodies.size()); i++) {
            Comment c = new Comment();
            c.author = authors.get(i); c.body = bodies.get(i);
            if (i < metas.size()) c.meta = metas.get(i).replace(" 回复", "");
            if (!c.author.isEmpty() && !c.body.isEmpty()) out.add(c);
        }
        return out;
    }

    private int mergeComments(List<Comment> visible) {
        synchronized (dataLock) { return mergeCommentList(comments, visible); }
    }

    private List<Comment> readCommentsSnapshot(int target, int maxPasses) {
        ArrayList<Comment> result = new ArrayList<>();
        int stale = 0;
        for (int pass = 0; pass < maxPasses; pass++) {
            int added = mergeCommentList(result,
                    callOnMain(this::readVisibleComments, new ArrayList<>()));
            stale = added == 0 ? stale + 1 : 0;
            if (result.size() >= target || stale >= 3 || !scrollNoteForward()) break;
            pause(420);
        }
        return result;
    }

    private static int mergeCommentList(List<Comment> target, List<Comment> incoming) {
        int added = 0;
        for (Comment c : incoming) {
            boolean duplicate = false;
            for (Comment old : target) {
                if (old.author.equals(c.author) && old.body.equals(c.body)) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) { target.add(c); added++; }
        }
        return added;
    }

    private boolean imageVisible() { return visibleImageBounds() != null; }

    private List<byte[]> cacheVisibleImages(int count) {
        ArrayList<byte[]> result = new ArrayList<>();
        if (count <= 0) return result;
        for (int i = 0; i < 8 && !callOnMain(this::imageVisible, false); i++) {
            service.swipeScreen(540, 520, 540, 2050, 360);
            pause(350);
        }
        if (!callOnMain(this::imageVisible, false)) return result;
        int capped = Math.min(count, 12);
        for (int target = 0; target < capped; target++) {
            for (int move = 0; move < count + 1; move++) {
                int current = callOnMain(this::visibleImageIndex, 0);
                if (current == target) break;
                final boolean forward = current < target;
                if (!callOnMain(() -> scrollImage(forward), false)) break;
                pause(360);
            }
            byte[] bmp = captureVisibleImageBmp();
            result.add(bmp);
        }
        return result;
    }

    private byte[] captureVisibleImageBmp() {
        Rect bounds = callOnMain(this::visibleImageBounds, null);
        Bitmap screen = service.captureScreenBitmap();
        if (bounds == null || screen == null) return null;
        try {
            bounds.intersect(0, 0, screen.getWidth(), screen.getHeight());
            return bounds.width() > 0 && bounds.height() > 0 ? imageBmp(screen, bounds) : null;
        } finally {
            screen.recycle();
        }
    }

    private int visibleImageIndex() {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return 0;
        try { return findImagePosition(root)[0]; } finally { root.recycle(); }
    }

    private Rect visibleImageBounds() {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return null;
        try {
            AccessibilityNodeInfo image = findByIdSuffix(root, "/photoImageView");
            if (image == null) image = findByIdSuffix(root, "/imageListView");
            if (image == null) return null;
            try { Rect r = new Rect(); image.getBoundsInScreen(r); return r; }
            finally { image.recycle(); }
        } finally { root.recycle(); }
    }

    private boolean scrollImage(boolean forward) {
        return actionOnId("/imageListView", forward ? AccessibilityNodeInfo.ACTION_SCROLL_FORWARD
                : AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD);
    }

    private static byte[] imageBmp(Bitmap screen, Rect crop) {
        final int w = 480, h = 650;
        Bitmap fitted = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(fitted);
        canvas.drawColor(Color.WHITE);
        float scale = Math.min((float) w / crop.width(), (float) h / crop.height());
        int dw = Math.max(1, Math.round(crop.width() * scale));
        int dh = Math.max(1, Math.round(crop.height() * scale));
        int left = (w - dw) / 2, top = (h - dh) / 2;
        Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        canvas.drawBitmap(screen, crop, new Rect(left, top, left + dw, top + dh), paint);
        int[] pixels = new int[w * h];
        fitted.getPixels(pixels, 0, w, 0, 0, w, h);
        fitted.recycle();
        int[] error = new int[w * h];
        int rowBytes = ((w + 31) / 32) * 4;
        ByteBuffer header = ByteBuffer.allocate(62).order(ByteOrder.LITTLE_ENDIAN);
        header.put((byte) 'B').put((byte) 'M').putInt(62 + rowBytes * h).putInt(0).putInt(62);
        header.putInt(40).putInt(w).putInt(-h).putShort((short) 1).putShort((short) 1);
        header.putInt(0).putInt(rowBytes * h).putInt(2835).putInt(2835).putInt(2).putInt(0);
        header.putInt(0x00000000).putInt(0x00ffffff);
        ByteArrayOutputStream out = new ByteArrayOutputStream(62 + rowBytes * h);
        out.write(header.array(), 0, header.array().length);
        byte[] row = new byte[rowBytes];
        for (int y = 0; y < h; y++) {
            java.util.Arrays.fill(row, (byte) 0);
            for (int x = 0; x < w; x++) {
                int i = y * w + x, c = pixels[i];
                int gray = (30 * ((c >> 16) & 255) + 59 * ((c >> 8) & 255) + 11 * (c & 255)) / 100;
                int value = Math.max(0, Math.min(255, gray + error[i]));
                boolean white = value >= 140;
                if (white) row[x >> 3] |= 0x80 >> (x & 7);
                int q = value - (white ? 255 : 0);
                if (x + 1 < w) error[i + 1] += q * 7 / 16;
                if (y + 1 < h) {
                    if (x > 0) error[i + w - 1] += q * 3 / 16;
                    error[i + w] += q * 5 / 16;
                    if (x + 1 < w) error[i + w + 1] += q / 16;
                }
            }
            out.write(row, 0, row.length);
        }
        return out.toByteArray();
    }

    private boolean actionOnId(String suffix, int action) {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return false;
        try {
            AccessibilityNodeInfo node = findByIdSuffix(root, suffix);
            if (node == null) return false;
            try { return node.performAction(action); } finally { node.recycle(); }
        } finally { root.recycle(); }
    }

    private boolean clickByIdSuffix(String suffix) { return clickNode(findRootNodeById(suffix)); }

    private boolean clickByDescription(String description) {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return false;
        try { return clickNode(findByDescription(root, description)); } finally { root.recycle(); }
    }

    private boolean clickByText(String text) {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return false;
        try { return clickNode(findByText(root, text)); } finally { root.recycle(); }
    }

    private AccessibilityNodeInfo findRootNodeById(String suffix) {
        AccessibilityNodeInfo root = service.getRootInActiveWindow();
        if (root == null) return null;
        try { return findByIdSuffix(root, suffix); } finally { root.recycle(); }
    }

    private static boolean clickNode(AccessibilityNodeInfo node) {
        if (node == null) return false;
        try {
            AccessibilityNodeInfo clickable = findClickable(node);
            if (clickable != null) {
                try { return clickable.performAction(AccessibilityNodeInfo.ACTION_CLICK); }
                finally { clickable.recycle(); }
            }
            AccessibilityNodeInfo parent = node.getParent();
            for (int i = 0; parent != null && i < 4; i++) {
                if (parent.isClickable()) {
                    try { return parent.performAction(AccessibilityNodeInfo.ACTION_CLICK); }
                    finally { parent.recycle(); }
                }
                AccessibilityNodeInfo next = parent.getParent();
                parent.recycle();
                parent = next;
            }
            return false;
        } finally { node.recycle(); }
    }

    private void collectValues(AccessibilityNodeInfo node, String suffix, List<String> out) {
        if (idOf(node).endsWith(suffix) && !textOf(node).isEmpty()) out.add(textOf(node));
        forEachChild(node, child -> collectValues(child, suffix, out));
    }

    private static boolean hasId(AccessibilityNodeInfo node, String suffix) {
        AccessibilityNodeInfo found = findByIdSuffix(node, suffix);
        if (found == null) return false;
        found.recycle();
        return true;
    }

    private static boolean hasDescription(AccessibilityNodeInfo node, String wanted) {
        if (wanted.equals(descriptionOf(node))) return true;
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            try { if (hasDescription(child, wanted)) return true; } finally { child.recycle(); }
        }
        return false;
    }

    private static AccessibilityNodeInfo findByIdSuffix(AccessibilityNodeInfo node, String suffix) {
        if (idOf(node).endsWith(suffix)) return AccessibilityNodeInfo.obtain(node);
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found;
            try { found = findByIdSuffix(child, suffix); } finally { child.recycle(); }
            if (found != null) return found;
        }
        return null;
    }

    private static AccessibilityNodeInfo findByDescription(AccessibilityNodeInfo node, String description) {
        if (description.equals(descriptionOf(node))) return AccessibilityNodeInfo.obtain(node);
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found;
            try { found = findByDescription(child, description); } finally { child.recycle(); }
            if (found != null) return found;
        }
        return null;
    }

    private static AccessibilityNodeInfo findByText(AccessibilityNodeInfo node, String text) {
        if (text.equals(textOf(node))) return AccessibilityNodeInfo.obtain(node);
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found;
            try { found = findByText(child, text); } finally { child.recycle(); }
            if (found != null) return found;
        }
        return null;
    }

    private static AccessibilityNodeInfo findClickable(AccessibilityNodeInfo node) {
        if (node.isClickable()) return AccessibilityNodeInfo.obtain(node);
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            AccessibilityNodeInfo found;
            try { found = findClickable(child); } finally { child.recycle(); }
            if (found != null) return found;
        }
        return null;
    }

    private static int[] findImagePosition(AccessibilityNodeInfo node) {
        int[] own = parseImagePosition(descriptionOf(node));
        if (own[1] > 0) return own;
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            int[] found;
            try { found = findImagePosition(child); } finally { child.recycle(); }
            if (found[1] > 0) return found;
        }
        return new int[]{0, 0};
    }

    private static int[] parseImagePosition(String value) {
        int at = value.indexOf("第"), comma = value.indexOf("张", at + 1), total = value.indexOf("共", comma + 1), end = value.indexOf("张", total + 1);
        if (at < 0 || comma < 0 || total < 0 || end < 0) return new int[]{0, 0};
        int current = intValue(value.substring(at + 1, comma));
        int count = intValue(value.substring(total + 1, end));
        return new int[]{Math.max(0, current - 1), count};
    }

    private boolean waitForPage(Page wanted, long timeoutMs) {
        long end = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < end) {
            Page p = callOnMain(this::detectPage, Page.OUTSIDE);
            if (p == wanted || (wanted == Page.NOTE && p == Page.COMMENTS)) return true;
            pause(180);
        }
        return false;
    }

    private interface NodeConsumer { void accept(AccessibilityNodeInfo node); }

    private static void forEachChild(AccessibilityNodeInfo node, NodeConsumer action) {
        for (int i = 0; i < node.getChildCount(); i++) {
            AccessibilityNodeInfo child = node.getChild(i);
            if (child == null) continue;
            try { action.accept(child); } finally { child.recycle(); }
        }
    }

    private <T> T callOnMain(java.util.concurrent.Callable<T> action, T fallback) {
        if (android.os.Looper.myLooper() == main.getLooper()) {
            try { return action.call(); } catch (Exception ignored) { return fallback; }
        }
        CountDownLatch done = new CountDownLatch(1);
        Object[] value = {fallback};
        main.post(() -> { try { value[0] = action.call(); } catch (Exception ignored) { } finally { done.countDown(); } });
        try { if (!done.await(4, TimeUnit.SECONDS)) return fallback; }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); return fallback; }
        @SuppressWarnings("unchecked") T result = (T) value[0];
        return result;
    }

    private static Note copyNote(Note source) {
        Note n = new Note();
        n.title = source.title; n.author = source.author; n.body = source.body;
        n.commentCount = source.commentCount; n.likes = source.likes; n.collects = source.collects;
        n.timestamp = source.timestamp; n.imageCount = source.imageCount; n.imageIndex = source.imageIndex;
        return n;
    }

    private static boolean sameFeed(FeedEntry a, FeedEntry b) { return a.title.equals(b.title) && a.author.equals(b.author); }
    private static String idOf(AccessibilityNodeInfo n) { String s = n.getViewIdResourceName(); return s == null ? "" : s; }
    private static String textOf(AccessibilityNodeInfo n) { CharSequence s = n.getText(); return s == null ? "" : s.toString().trim(); }
    private static String descriptionOf(AccessibilityNodeInfo n) { CharSequence s = n.getContentDescription(); return s == null ? "" : s.toString().trim(); }
    private static String pageName(Page p) { return p.name().toLowerCase(java.util.Locale.ROOT); }
    private String activityName(Page p) {
        String actual = service.foregroundClassSnapshot();
        if (!actual.isEmpty() && actual.contains("xingin")) return actual;
        if (p == Page.HOME) return "com.xingin.xhs.index.v2.IndexActivityV2";
        if (p == Page.NOTE || p == Page.COMMENTS) return "com.xingin.matrix.notedetail.NoteDetailActivity";
        return actual;
    }
    private static void pause(long ms) { try { Thread.sleep(ms); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    private static String digits(String value) { StringBuilder b = new StringBuilder(); for (int i = 0; i < value.length(); i++) if (Character.isDigit(value.charAt(i))) b.append(value.charAt(i)); return b.toString(); }
    private static int intValue(String value) { try { return Integer.parseInt(digits(value)); } catch (Exception e) { return 0; } }
    private static String decode(String value) { try { return URLDecoder.decode(value == null ? "" : value, StandardCharsets.UTF_8.name()); } catch (Exception e) { return ""; } }

    static String json(String value) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c == '\n') out.append("\\n");
            else if (c == '\r') out.append("\\r");
            else if (c == '\t') out.append("\\t");
            else if (c < 0x20) out.append(String.format("\\u%04x", (int) c));
            else out.append(c);
        }
        return out.toString();
    }
}
