package com.murphy.m4screenbridge.browser.session;

/**
 * Product-facing Browser Bridge connection state.
 *
 * <p>This class deliberately contains no Android or socket code. Discovery and transport remain
 * responsible for doing work; they report deterministic events here so UI/notification/dumpsys do
 * not have to infer state from a collection of booleans and free-form strings.</p>
 */
public final class BrowserConnectionState {
    public enum State {
        DISABLED,
        DISCOVERING,
        CONNECTING,
        CONNECTED,
        RECONNECTING,
        ERROR
    }

    public enum Mode {
        NONE,
        AUTO,
        MANUAL,
        LOOPBACK
    }

    public enum Source {
        NONE,
        MANUAL,
        DISCOVERED,
        CACHED,
        LOOPBACK
    }

    public static final class Snapshot {
        public final State state;
        public final Mode mode;
        public final Source source;
        public final String host;
        public final int port;
        public final long generation;
        public final long reconnects;
        public final String error;

        Snapshot(State state, Mode mode, Source source, String host, int port,
                long generation, long reconnects, String error) {
            this.state = state;
            this.mode = mode;
            this.source = source;
            this.host = host;
            this.port = port;
            this.generation = generation;
            this.reconnects = reconnects;
            this.error = error;
        }

        public boolean hasEndpoint() {
            return source == Source.LOOPBACK || (!host.isEmpty() && port >= 1 && port <= 65535);
        }

        public String endpoint() {
            if (source == Source.LOOPBACK) return "loopback";
            return hasEndpoint() ? host + ":" + port : "";
        }

        public String summary() {
            StringBuilder out = new StringBuilder();
            out.append(state.name().toLowerCase())
                    .append(" mode=").append(mode.name().toLowerCase())
                    .append(" source=").append(source.name().toLowerCase());
            String ep = endpoint();
            if (!ep.isEmpty()) out.append(" endpoint=").append(ep);
            if (reconnects > 0) out.append(" reconnects=").append(reconnects);
            if (!error.isEmpty()) out.append(" error=").append(error);
            return out.toString();
        }
    }

    private State state = State.DISABLED;
    private Mode mode = Mode.NONE;
    private Source source = Source.NONE;
    private String host = "";
    private int port;
    private long generation;
    private long reconnects;
    private String error = "";

    public void stop() {
        generation++;
        state = State.DISABLED;
        mode = Mode.NONE;
        source = Source.NONE;
        host = "";
        port = 0;
        reconnects = 0;
        error = "";
    }

    public void startAuto() {
        begin(Mode.AUTO);
        state = State.DISCOVERING;
    }

    public void startManual(String host, int port) {
        validateTcpEndpoint(host, port);
        begin(Mode.MANUAL);
        selectEndpoint(Source.MANUAL, host, port);
    }

    public void startLoopback() {
        begin(Mode.LOOPBACK);
        source = Source.LOOPBACK;
        host = "";
        port = 0;
        state = State.CONNECTING;
    }

    /** AUTO discovery selected cached or newly resolved endpoint. */
    public void selectAutoEndpoint(Source source, String host, int port) {
        if (mode != Mode.AUTO) throw new IllegalStateException("not in AUTO mode");
        if (source != Source.CACHED && source != Source.DISCOVERED) {
            throw new IllegalArgumentException("AUTO endpoint source must be cached/discovered");
        }
        selectEndpoint(source, host, port);
    }

    /** AUTO currently has no usable endpoint but discovery remains live. */
    public void clearAutoEndpoint(String reason) {
        if (mode != Mode.AUTO) throw new IllegalStateException("not in AUTO mode");
        source = Source.NONE;
        host = "";
        port = 0;
        state = State.DISCOVERING;
        if (reason != null && !reason.trim().isEmpty()) error = reason.trim();
    }

    /** The transport has started/restarted a connect attempt for the selected endpoint. */
    public void connecting() {
        requireStarted();
        if (!hasEndpoint()) {
            if (mode == Mode.AUTO) {
                state = State.DISCOVERING;
                return;
            }
            throw new IllegalStateException("connecting without endpoint");
        }
        state = State.CONNECTING;
    }

    public void connected() {
        requireStarted();
        if (!hasEndpoint()) throw new IllegalStateException("connected without endpoint");
        state = State.CONNECTED;
        error = "";
    }

    /**
     * Transport loss. A retrying transport exposes RECONNECTING; exhausted/non-retrying failures
     * become ERROR, except AUTO with no endpoint which returns to DISCOVERING.
     */
    public void disconnected(String reason, boolean willRetry) {
        requireStarted();
        if (reason != null && !reason.trim().isEmpty()) error = reason.trim();
        if (willRetry && hasEndpoint()) {
            reconnects++;
            state = State.RECONNECTING;
        } else if (mode == Mode.AUTO && !hasEndpoint()) {
            state = State.DISCOVERING;
        } else {
            state = State.ERROR;
        }
    }

    /** Discovery problems are diagnostic while AUTO discovery is still allowed to recover. */
    public void discoveryError(String message) {
        if (mode != Mode.AUTO) return;
        if (message != null && !message.trim().isEmpty()) error = message.trim();
        if (!hasEndpoint() && state != State.DISABLED) state = State.DISCOVERING;
    }

    public void fatal(String message) {
        requireStarted();
        error = message == null ? "" : message.trim();
        state = State.ERROR;
    }

    public Snapshot snapshot() {
        return new Snapshot(state, mode, source, host, port, generation, reconnects, error);
    }

    private void begin(Mode newMode) {
        generation++;
        mode = newMode;
        source = Source.NONE;
        host = "";
        port = 0;
        reconnects = 0;
        error = "";
        state = State.CONNECTING;
    }

    private void selectEndpoint(Source newSource, String newHost, int newPort) {
        if (newSource == Source.NONE || newSource == Source.LOOPBACK) {
            throw new IllegalArgumentException("invalid TCP endpoint source");
        }
        validateTcpEndpoint(newHost, newPort);
        source = newSource;
        host = newHost.trim();
        port = newPort;
        state = State.CONNECTING;
        error = "";
    }

    private static void validateTcpEndpoint(String host, int port) {
        if (host == null || host.trim().isEmpty()) throw new IllegalArgumentException("host is empty");
        if (port < 1 || port > 65535) throw new IllegalArgumentException("invalid port");
    }

    private boolean hasEndpoint() {
        return source == Source.LOOPBACK || (!host.isEmpty() && port >= 1 && port <= 65535);
    }

    private void requireStarted() {
        if (mode == Mode.NONE || state == State.DISABLED) {
            throw new IllegalStateException("Browser Bridge is disabled");
        }
    }
}
