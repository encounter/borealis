package dev.encounter.borealis;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import javax.net.ssl.HttpsURLConnection;

public final class BorealisHttpClient {
    public static final int ERROR_NONE = 0;
    public static final int ERROR_INVALID_URL = 1;
    public static final int ERROR_UNSUPPORTED_SCHEME = 2;
    public static final int ERROR_TIMEOUT = 3;
    public static final int ERROR_CANCELED = 4;
    public static final int ERROR_NETWORK = 5;

    private static final int MAX_REDIRECTS = 5;
    private static final int READ_BUFFER_SIZE = 64 * 1024;

    public static final class Response {
        public int error;
        public String message;

        Response(int error, String message) {
            this.error = error;
            this.message = message;
        }
    }

    private BorealisHttpClient() {
    }

    public static Response request(String method, String url, String[] headerNames,
                                   String[] headerValues, byte[] requestBody,
                                   int connectTimeoutMs, int readTimeoutMs, long totalTimeoutMs,
                                   long observerAddress, long signalsAddress) {
        if (url == null || url.isEmpty()) {
            return fail(ERROR_INVALID_URL, "URL is empty");
        }

        try {
            long startTimeNs = System.nanoTime();
            URL currentUrl = new URL(url);
            if (!isHttps(currentUrl)) {
                return fail(ERROR_UNSUPPORTED_SCHEME, "Only https:// URLs are supported");
            }

            String currentMethod = "POST".equals(method) ? "POST" : "GET";
            byte[] currentBody = requestBody != null ? requestBody : new byte[0];
            for (int redirect = 0; redirect <= MAX_REDIRECTS; ++redirect) {
                if (isCanceled(signalsAddress)) {
                    return fail(ERROR_CANCELED, "Request canceled");
                }

                HttpsURLConnection connection = (HttpsURLConnection) currentUrl.openConnection();
                try {
                    connection.setRequestMethod(currentMethod);
                    connection.setConnectTimeout(boundedTimeout(
                            connectTimeoutMs, startTimeNs, totalTimeoutMs));
                    connection.setReadTimeout(boundedTimeout(
                            readTimeoutMs, startTimeNs, totalTimeoutMs));
                    connection.setUseCaches(false);
                    connection.setInstanceFollowRedirects(false);
                    applyHeaders(connection, headerNames, headerValues);
                    if ("POST".equals(currentMethod)) {
                        writeRequestBody(connection, currentBody, signalsAddress, startTimeNs,
                                totalTimeoutMs);
                    }

                    int statusCode = connection.getResponseCode();
                    checkTotalTimeout(startTimeNs, totalTimeoutMs);
                    if (isRedirect(statusCode)) {
                        String location = connection.getHeaderField("Location");
                        if (location == null || location.isEmpty()) {
                            return fail(ERROR_NETWORK,
                                    "Redirect response did not include Location");
                        }

                        URL nextUrl = new URL(currentUrl, location);
                        if (!isHttps(nextUrl)) {
                            return fail(ERROR_UNSUPPORTED_SCHEME,
                                    "Only https:// redirects are supported");
                        }
                        if (statusCode != 307 && statusCode != 308) {
                            currentMethod = "GET";
                            currentBody = new byte[0];
                        }
                        currentUrl = nextUrl;
                        continue;
                    }

                    HeaderLists headers = readHeaders(connection);
                    if (onResponse(observerAddress, statusCode, headers.names, headers.values)) {
                        return success();
                    }
                    return readBody(connection, statusCode, readTimeoutMs, totalTimeoutMs,
                            startTimeNs, observerAddress, signalsAddress);
                } finally {
                    connection.disconnect();
                }
            }
            return fail(ERROR_NETWORK, "Too many redirects");
        } catch (MalformedURLException e) {
            return fail(ERROR_INVALID_URL, "Failed to parse URL");
        } catch (SocketTimeoutException e) {
            return fail(ERROR_TIMEOUT, "Request timed out");
        } catch (RequestCanceledException e) {
            return fail(ERROR_CANCELED, "Request canceled");
        } catch (IOException e) {
            String message = e.getMessage();
            return fail(ERROR_NETWORK, message != null ? message : e.toString());
        } catch (ClassCastException e) {
            return fail(ERROR_UNSUPPORTED_SCHEME, "Only https:// URLs are supported");
        }
    }

    private static void writeRequestBody(HttpsURLConnection connection, byte[] body,
                                         long signalsAddress, long startTimeNs,
                                         long totalTimeoutMs)
            throws IOException, RequestCanceledException {
        connection.setDoOutput(true);
        connection.setFixedLengthStreamingMode(body.length);
        checkCanceled(signalsAddress);
        try (OutputStream output = connection.getOutputStream()) {
            checkTotalTimeout(startTimeNs, totalTimeoutMs);
            output.write(body);
            checkTotalTimeout(startTimeNs, totalTimeoutMs);
            checkCanceled(signalsAddress);
        }
    }

    private static Response readBody(HttpsURLConnection connection, int statusCode,
                                     int readTimeoutMs, long totalTimeoutMs, long startTimeNs,
                                     long observerAddress, long signalsAddress)
            throws IOException, RequestCanceledException {
        InputStream stream = statusCode >= HttpURLConnection.HTTP_BAD_REQUEST ?
                connection.getErrorStream() : connection.getInputStream();
        if (stream == null) {
            return success();
        }

        byte[] buffer = new byte[READ_BUFFER_SIZE];
        try (InputStream bodyStream = stream) {
            while (true) {
                checkCanceled(signalsAddress);
                connection.setReadTimeout(
                        boundedTimeout(readTimeoutMs, startTimeNs, totalTimeoutMs));
                int read = bodyStream.read(buffer);
                if (read < 0) {
                    return success();
                }
                if (read == 0) {
                    continue;
                }
                if (onData(observerAddress, buffer, read)) {
                    return success();
                }
            }
        }
    }

    private static int boundedTimeout(int timeoutMs, long startTimeNs, long totalTimeoutMs)
            throws SocketTimeoutException {
        if (totalTimeoutMs <= 0) {
            return Math.max(timeoutMs, 1);
        }

        long elapsedMs = (System.nanoTime() - startTimeNs) / 1_000_000L;
        long remainingMs = totalTimeoutMs - elapsedMs;
        if (remainingMs <= 0) {
            throw new SocketTimeoutException("Request timed out");
        }
        return (int) Math.max(1, Math.min(Math.max(timeoutMs, 1), remainingMs));
    }

    private static void checkTotalTimeout(long startTimeNs, long totalTimeoutMs)
            throws SocketTimeoutException {
        boundedTimeout(Integer.MAX_VALUE, startTimeNs, totalTimeoutMs);
    }

    private static void checkCanceled(long signalsAddress) throws RequestCanceledException {
        if (isCanceled(signalsAddress)) {
            throw new RequestCanceledException();
        }
    }

    private static void applyHeaders(HttpsURLConnection connection, String[] names,
                                     String[] values) {
        if (names == null || values == null) {
            return;
        }

        int count = Math.min(names.length, values.length);
        for (int i = 0; i < count; ++i) {
            if (names[i] != null && values[i] != null) {
                connection.setRequestProperty(names[i], values[i]);
            }
        }
    }

    private static boolean isHttps(URL url) {
        return "https".equalsIgnoreCase(url.getProtocol());
    }

    private static boolean isRedirect(int statusCode) {
        return statusCode == HttpURLConnection.HTTP_MOVED_PERM ||
                statusCode == HttpURLConnection.HTTP_MOVED_TEMP ||
                statusCode == HttpURLConnection.HTTP_SEE_OTHER ||
                statusCode == 307 ||
                statusCode == 308;
    }

    private static HeaderLists readHeaders(HttpsURLConnection connection) {
        List<String> names = new ArrayList<>();
        List<String> values = new ArrayList<>();

        Map<String, List<String>> headerFields = connection.getHeaderFields();
        if (headerFields == null) {
            return new HeaderLists(new String[0], new String[0]);
        }

        for (Map.Entry<String, List<String>> entry : headerFields.entrySet()) {
            String name = entry.getKey();
            if (name == null) {
                continue;
            }
            List<String> entryValues = entry.getValue();
            if (entryValues == null || entryValues.isEmpty()) {
                names.add(name);
                values.add("");
                continue;
            }
            for (String value : entryValues) {
                names.add(name);
                values.add(value != null ? value : "");
            }
        }

        return new HeaderLists(names.toArray(new String[0]), values.toArray(new String[0]));
    }

    private static Response success() {
        return new Response(ERROR_NONE, "");
    }

    private static Response fail(int error, String message) {
        return new Response(error, message);
    }

    private static native boolean onResponse(long observerAddress, int statusCode,
                                             String[] headerNames, String[] headerValues);

    private static native boolean onData(long observerAddress, byte[] data, int length);

    private static native boolean isCanceled(long signalsAddress);

    private static final class HeaderLists {
        final String[] names;
        final String[] values;

        HeaderLists(String[] names, String[] values) {
            this.names = names;
            this.values = values;
        }
    }

    private static final class RequestCanceledException extends Exception {
        private static final long serialVersionUID = 1L;
    }
}
