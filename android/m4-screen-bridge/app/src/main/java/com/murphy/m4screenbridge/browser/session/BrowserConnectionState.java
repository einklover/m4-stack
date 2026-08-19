package com.murphy.m4screenbridge.browser.session;

/**
 * Thread-safe product-facing Browser Bridge connection state.
 *
 * <p>This class deliberately contains no Android or socket code. Discovery and transport remain
 * responsible for doing work; they report deterministic events here so UI/notification/dumpsys do
 * not have to infer state from a collection of booleans and free-form strings.</p>
 */
public final class BrowserConnectionState {
    public enum State { DISABLED, DISCOVERING, CONNECTING, CONNECTED, RECONNECTING, ERROR }
    public enum Mode { NONE, AUTO, MANUAL, LOOPBACK }
    public enum Source { NONE, MANUAL, DISCOVERED, CACHED, LOOPBACK }

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

    public synchronized void stop() {
        generation++;
        state = State.DISABLED;
        mode = Mode.NONE;
        source = Source.NONE;
        host = "";
        port = 0;
        reconnects = 0;
        error = "";
    }

    public synchronized void startAuto() {
        begin(Mode.AUTO);
        state = State.DISCOVERING;
    }

    public synchronized void startManual(String host, int port) {
        validateTcpEndpoint(host, port);
        begin(Mode.MANUAL);
        selectEndpoint(Source.MANUAL, host, port);
    }

    public synchronized void startLoopback() {
        begin(Mode.LOOPBACK);
        source = Source.LOOPBACK;
        host = "";
        port = 0;
        state = State.CONNECTING;
    }

    /** AUTO discovery selected cached or newly resolved endpoint. */
    public synchronized void selectAutoEndpoint(Source newSource, String newHost, int newPort) {
        if (mode != Mode.AUTO) throw new IllegalStateException("not in AUTO mode");
        if (newSource != Source.CACHED && newSource != Source.DISCOVERED) {
            throw new IllegalArgumentException("AUTO endpoint source must be cached/discovered");
        }
        validateTcpEndpoint(newHost, newPort);
        boolean sameEndpoint = newHost.trim().equalsIgnoreCase(host) && newPort == port && hasEndpoint();
        State previous = state;
        selectEndpoint(newSource, newHost, newPort);
        // mDNS commonly promotes an already-connected cached endpoint to DISCOVERED. That is a
        // metadata improvement, not a new TCP attempt, so preserve the transport state.
        if (sameEndpoint && (previous == State.CONNECTED || previous == State.RECONNECTING)) {
            state = previous;
            if (state == State.CONNECTED) error = "";
        }
    }

    /** AUTO currently has no usable endpoint but discovery remains live. */
    public synchronized void clearAutoEndpoint(String reason) {
        if (mode != Mode.AUTO) throw new IllegalStateException("not in AUTO mode");
        source = Source.NONE;
        host = "";
        port = 0;
        state = State.DISCOVERING;
        if (reason != null && !reason.trim().isEmpty()) error = reason.trim();
    }

    /** The transport has started a first connect attempt for the selected endpoint. */
    public synchronized void connecting() {
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

    public synchronized void connected() {
        requireStarted();
        if (!hasEndpoint()) throw new IllegalStateException("connected without endpoint");
        state = State.CONNECTED;
        error = "";
    }

    /** Transport loss or a failed connect attempt. */
    public synchronized void disconnected(String reason, boolean willRetry) {
        requireStarted();
        if (reason != null && !reason.trim().isEmpty()) error = reason.trim();
        if (willRetry && hasEndpoint()) {
            // A socket failure can produce both onError and onDisconnected. Count once.
            if (state != State.RECONNECTING) reconnects++;
            state = State.RECONNECTING;
        } else if (mode == Mode.AUTO && !hasEndpoint()) {
            state = State.DISCOVERING;
        } else {
            state = State.ERROR;
        }
    }

    /** Discovery problems are diagnostic while AUTO discovery is still allowed to recover. */
    public synchronized void discoveryError(String message) {
        if (mode != Mode.AUTO) return;
        if (message != null && !message.trim().isEmpty()) error = message.trim();
        if (!hasEndpoint() && state != State.DISABLED) state = State.DISCOVERING;
    }

    public synchronized void fatal(String message) {
        requireStarted();
        error = message == null ? "" : message.trim();
        state = State.ERROR;
    }

    public synchronized Snapshot snapshot() {
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
