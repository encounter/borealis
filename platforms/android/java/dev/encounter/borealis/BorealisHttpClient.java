package dev.encounter.borealis;

import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import okhttp3.Call;
import okhttp3.Callback;
import okhttp3.Dispatcher;
import okhttp3.Headers;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.ResponseBody;

public final class BorealisHttpClient {
    public static final int ERROR_NONE = 0;
    public static final int ERROR_INVALID_URL = 1;
    public static final int ERROR_UNSUPPORTED_SCHEME = 2;
    public static final int ERROR_TIMEOUT = 3;
    public static final int ERROR_CANCELED = 4;
    public static final int ERROR_NETWORK = 5;

    private static final int CANCEL_POLL_INTERVAL_MS = 50;
    private static final int READ_BUFFER_SIZE = 64 * 1024;
    private static final int MAX_REQUESTS_PER_HOST = 8;
    private static final OkHttpClient HTTP_CLIENT = createHttpClient();

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

        Request.Builder requestBuilder = new Request.Builder();
        try {
            requestBuilder.url(url);
        } catch (IllegalArgumentException e) {
            return fail(ERROR_INVALID_URL, "Failed to parse URL");
        }

        final Request httpRequest;
        try {
            applyHeaders(requestBuilder, headerNames, headerValues);
            byte[] body = requestBody != null ? requestBody : new byte[0];
            requestBuilder.method(method,
                    methodHasRequestBody(method) ? RequestBody.create(body, null) : null);
            httpRequest = requestBuilder.build();
        } catch (IllegalArgumentException e) {
            String message = e.getMessage();
            return fail(ERROR_NETWORK, message != null ? message : e.toString());
        }

        if (!httpRequest.url().isHttps()) {
            return fail(ERROR_UNSUPPORTED_SCHEME, "Only https:// URLs are supported");
        }

        OkHttpClient client = HTTP_CLIENT.newBuilder()
                .connectTimeout(Math.max(connectTimeoutMs, 1), TimeUnit.MILLISECONDS)
                .readTimeout(Math.max(readTimeoutMs, 1), TimeUnit.MILLISECONDS)
                .writeTimeout(Math.max(readTimeoutMs, 1), TimeUnit.MILLISECONDS)
                .callTimeout(Math.max(totalTimeoutMs, 0), TimeUnit.MILLISECONDS)
                .build();
        Call call = client.newCall(httpRequest);
        RequestCompletion completion = new RequestCompletion(call);

        try {
            call.enqueue(new Callback() {
                @Override
                public void onFailure(Call failedCall, IOException exception) {
                    completion.finish(failure(completion, exception));
                }

                @Override
                public void onResponse(Call completedCall, okhttp3.Response response) {
                    completion.finish(readResponse(completedCall, response, completion,
                            observerAddress, signalsAddress));
                }
            });
        } catch (RuntimeException e) {
            String message = e.getMessage();
            return fail(ERROR_NETWORK, message != null ? message : e.toString());
        }

        boolean interrupted = false;
        while (!completion.done()) {
            if (isCanceled(signalsAddress)) {
                completion.cancel();
            }
            try {
                completion.await(CANCEL_POLL_INTERVAL_MS);
            } catch (InterruptedException e) {
                interrupted = true;
                completion.cancel();
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
        return completion.response();
    }

    private static OkHttpClient createHttpClient() {
        Dispatcher dispatcher = new Dispatcher();
        dispatcher.setMaxRequestsPerHost(MAX_REQUESTS_PER_HOST);
        return new OkHttpClient.Builder()
                .dispatcher(dispatcher)
                .followSslRedirects(false)
                .build();
    }

    private static Response readResponse(Call call, okhttp3.Response httpResponse,
                                         RequestCompletion completion, long observerAddress,
                                         long signalsAddress) {
        try (okhttp3.Response response = httpResponse) {
            checkCanceled(completion, signalsAddress);
            HeaderLists headers = readHeaders(response.headers());
            if (onResponse(observerAddress, response.code(), headers.names, headers.values)) {
                call.cancel();
                return success();
            }

            ResponseBody responseBody = response.body();
            if (responseBody == null) {
                return success();
            }
            return readBody(call, responseBody, completion, observerAddress, signalsAddress);
        } catch (RequestCanceledException e) {
            return fail(ERROR_CANCELED, "Request canceled");
        } catch (InterruptedIOException e) {
            return failure(completion, e);
        } catch (IOException e) {
            return failure(completion, e);
        } catch (RuntimeException e) {
            String message = e.getMessage();
            return fail(ERROR_NETWORK, message != null ? message : e.toString());
        }
    }

    private static Response readBody(Call call, ResponseBody responseBody,
                                     RequestCompletion completion, long observerAddress,
                                     long signalsAddress)
            throws IOException, RequestCanceledException {
        byte[] buffer = new byte[READ_BUFFER_SIZE];
        try (InputStream bodyStream = responseBody.byteStream()) {
            while (true) {
                checkCanceled(completion, signalsAddress);
                int read = bodyStream.read(buffer);
                if (read < 0) {
                    return success();
                }
                if (read == 0) {
                    continue;
                }
                if (onData(observerAddress, buffer, read)) {
                    call.cancel();
                    return success();
                }
            }
        }
    }

    private static void checkCanceled(RequestCompletion completion, long signalsAddress)
            throws RequestCanceledException {
        if (completion.cancellationRequested() || isCanceled(signalsAddress)) {
            completion.cancel();
            throw new RequestCanceledException();
        }
    }

    private static Response failure(RequestCompletion completion, IOException exception) {
        if (completion.cancellationRequested()) {
            return fail(ERROR_CANCELED, "Request canceled");
        }
        if (exception instanceof InterruptedIOException) {
            return fail(ERROR_TIMEOUT, "Request timed out");
        }
        String message = exception.getMessage();
        return fail(ERROR_NETWORK, message != null ? message : exception.toString());
    }

    private static void applyHeaders(Request.Builder requestBuilder, String[] names,
                                     String[] values) {
        if (names == null || values == null) {
            return;
        }

        int count = Math.min(names.length, values.length);
        for (int i = 0; i < count; ++i) {
            if (names[i] != null && values[i] != null) {
                requestBuilder.addHeader(names[i], values[i]);
            }
        }
    }

    private static boolean methodHasRequestBody(String method) {
        return "POST".equals(method);
    }

    private static HeaderLists readHeaders(Headers headers) {
        String[] names = new String[headers.size()];
        String[] values = new String[headers.size()];
        for (int i = 0; i < headers.size(); ++i) {
            names[i] = headers.name(i);
            values[i] = headers.value(i);
        }
        return new HeaderLists(names, values);
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

    private static final class RequestCompletion {
        private final Call call;
        private final CountDownLatch latch = new CountDownLatch(1);
        private final AtomicBoolean cancellationRequested = new AtomicBoolean();
        private volatile Response response;

        RequestCompletion(Call call) {
            this.call = call;
        }

        void cancel() {
            cancellationRequested.set(true);
            call.cancel();
        }

        boolean cancellationRequested() {
            return cancellationRequested.get();
        }

        void finish(Response value) {
            response = value;
            latch.countDown();
        }

        boolean done() {
            return latch.getCount() == 0;
        }

        void await(long timeoutMs) throws InterruptedException {
            latch.await(timeoutMs, TimeUnit.MILLISECONDS);
        }

        Response response() {
            return response != null ? response : fail(ERROR_NETWORK,
                    "Android HTTP request did not return a response");
        }
    }

    private static final class RequestCanceledException extends Exception {
        private static final long serialVersionUID = 1L;
    }
}
