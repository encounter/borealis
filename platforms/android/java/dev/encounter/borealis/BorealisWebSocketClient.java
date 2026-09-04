package dev.encounter.borealis;

import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

import okhttp3.Headers;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

public final class BorealisWebSocketClient {
    private static final OkHttpClient BASE_CLIENT = new OkHttpClient.Builder()
            .followRedirects(false)
            .followSslRedirects(false)
            .build();
    private static final ScheduledExecutorService COMPLETIONS =
            Executors.newSingleThreadScheduledExecutor();
    private static final Map<Long, Connection> CONNECTIONS = new ConcurrentHashMap<>();

    private BorealisWebSocketClient() {
    }

    public static boolean connect(long token, String url, String[] headerNames,
                                  String[] headerValues, int connectTimeoutMs,
                                  int pingIntervalMs) {
        try {
            Request.Builder request = new Request.Builder().url(url);
            int headerCount = Math.min(headerNames.length, headerValues.length);
            for (int index = 0; index < headerCount; ++index) {
                request.addHeader(headerNames[index], headerValues[index]);
            }
            OkHttpClient.Builder client = BASE_CLIENT.newBuilder()
                    .connectTimeout(Math.max(connectTimeoutMs, 1), TimeUnit.MILLISECONDS);
            if (pingIntervalMs > 0) {
                client.pingInterval(pingIntervalMs, TimeUnit.MILLISECONDS);
            }
            Connection connection = new Connection(token);
            CONNECTIONS.put(token, connection);
            connection.socket = client.build().newWebSocket(request.build(), connection);
            return true;
        } catch (RuntimeException exception) {
            CONNECTIONS.remove(token);
            return false;
        }
    }

    public static boolean send(long token, int kind, byte[] data) {
        Connection connection = CONNECTIONS.get(token);
        return connection != null && connection.send(kind, data);
    }

    public static void close(long token, int code, String reason) {
        Connection connection = CONNECTIONS.get(token);
        if (connection != null) {
            connection.socket.close(code, reason);
        }
    }

    public static void abort(long token) {
        Connection connection = CONNECTIONS.remove(token);
        if (connection != null) {
            connection.active = false;
            connection.socket.cancel();
            try {
                connection.terminated.await(1, TimeUnit.SECONDS);
            } catch (InterruptedException exception) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private static final class Connection extends WebSocketListener {
        final long token;
        final ArrayDeque<Integer> pending = new ArrayDeque<>();
        final CountDownLatch terminated = new CountDownLatch(1);
        volatile WebSocket socket;
        volatile boolean active = true;
        long pendingBytes;
        boolean completionScheduled;

        Connection(long token) {
            this.token = token;
        }

        synchronized boolean send(int kind, byte[] data) {
            if (!active || socket == null) {
                return false;
            }
            boolean accepted = kind == 0
                    ? socket.send(new String(data, StandardCharsets.UTF_8))
                    : socket.send(ByteString.of(data));
            if (!accepted) {
                return false;
            }
            pending.addLast(data.length);
            pendingBytes += data.length;
            scheduleCompletionCheck();
            return true;
        }

        private synchronized void scheduleCompletionCheck() {
            if (completionScheduled || !active) {
                return;
            }
            completionScheduled = true;
            COMPLETIONS.schedule(this::checkCompletions, 10, TimeUnit.MILLISECONDS);
        }

        private void checkCompletions() {
            synchronized (this) {
                completionScheduled = false;
                if (!active || socket == null) {
                    return;
                }
                long queued = socket.queueSize();
                while (!pending.isEmpty() && queued <= pendingBytes - pending.peekFirst()) {
                    int bytes = pending.removeFirst();
                    pendingBytes -= bytes;
                    nativeOnSendComplete(token, bytes);
                }
                if (!pending.isEmpty()) {
                    scheduleCompletionCheck();
                }
            }
        }

        @Override
        public void onOpen(WebSocket webSocket, Response response) {
            if (!active) {
                return;
            }
            HeaderLists headers = readHeaders(response.headers());
            nativeOnOpen(token, response.header("Sec-WebSocket-Protocol", ""),
                    headers.names, headers.values);
        }

        @Override
        public void onMessage(WebSocket webSocket, String text) {
            if (active) {
                nativeOnMessage(token, 0, text.getBytes(StandardCharsets.UTF_8));
            }
        }

        @Override
        public void onMessage(WebSocket webSocket, ByteString bytes) {
            if (active) {
                nativeOnMessage(token, 1, bytes.toByteArray());
            }
        }

        @Override
        public void onClosing(WebSocket webSocket, int code, String reason) {
            if (active) {
                int replyCode = code == 1000 || (code >= 3000 && code <= 4999) ? code : 1000;
                webSocket.close(replyCode, reason);
            }
        }

        @Override
        public void onClosed(WebSocket webSocket, int code, String reason) {
            try {
                if (active) {
                    active = false;
                    CONNECTIONS.remove(token, this);
                    nativeOnClosed(token, code, reason);
                }
            } finally {
                terminated.countDown();
            }
        }

        @Override
        public void onFailure(WebSocket webSocket, Throwable throwable, Response response) {
            try {
                if (active) {
                    active = false;
                    CONNECTIONS.remove(token, this);
                    int status = response != null ? response.code() : 0;
                    String message = throwable.getMessage();
                    HeaderLists headers = response != null
                            ? readHeaders(response.headers())
                            : new HeaderLists(new String[0], new String[0]);
                    nativeOnFailure(token, message != null ? message : throwable.toString(), status,
                            headers.names, headers.values);
                }
                if (response != null) {
                    response.close();
                }
            } finally {
                terminated.countDown();
            }
        }
    }

    private static HeaderLists readHeaders(Headers headers) {
        String[] names = new String[headers.size()];
        String[] values = new String[headers.size()];
        for (int index = 0; index < headers.size(); ++index) {
            names[index] = headers.name(index);
            values[index] = headers.value(index);
        }
        return new HeaderLists(names, values);
    }

    private static final class HeaderLists {
        final String[] names;
        final String[] values;

        HeaderLists(String[] names, String[] values) {
            this.names = names;
            this.values = values;
        }
    }

    private static native void nativeOnOpen(
            long token, String protocol, String[] headerNames, String[] headerValues);
    private static native void nativeOnMessage(long token, int kind, byte[] data);
    private static native void nativeOnSendComplete(long token, long bytes);
    private static native void nativeOnClosed(long token, int code, String reason);
    private static native void nativeOnFailure(long token, String message, int status,
                                               String[] headerNames, String[] headerValues);
}
