#pragma once
// HTTP transport abstraction used by LLM providers. Production: WinHTTP
// (no third-party dependency). Tests: MockHttpTransport (no network, no keys).
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fledge::ai {

struct HttpRequest {
    std::string method = "POST";
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    // Longest wait for the answer. Replies are not streamed yet, so the server sends nothing until the whole reply is
    // written: a long answer from a large model can take several minutes. 120 s (the old value) cut those off with
    // WinHTTP error 12002 (timeout). Connecting has its own short limit in the transport.
    int timeoutMs = 1200000;  // 20 min: a full 32k-token reply from a slow model fits
};

struct HttpResponse {
    int status = 0;          // 0 = transport failure
    std::string body;
    std::string error;       // transport error text (never contains request headers)
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse send(const HttpRequest& request) = 0;
    // Streaming hook (SSE). Default: STUB - performs a normal request and
    // delivers the whole body as one chunk.
    virtual HttpResponse sendStreaming(const HttpRequest& request, const std::function<void(const std::string&)>& onChunk) {
        HttpResponse r = send(request);
        if (onChunk && r.status >= 200 && r.status < 300) onChunk(r.body);
        return r;
    }
};

// Platform transport (WinHTTP on Windows; returns a failing stub elsewhere).
std::unique_ptr<HttpTransport> createDefaultHttpTransport();

// Plain-language text for the WinHTTP error codes users actually meet (12002 timeout, 12007 name not found, ...).
std::string describeTransportError(unsigned long winHttpError);

// Retries failed requests with a growing delay (1 s, 2 s, 4 s, ... up to maxDelayMs, plus a little jitter).
// Retried: transport failures (status 0), 408, 409, 425, 429 and 5xx. Never retried: other 4xx (bad key, bad request).
// A timeout is retried at most `maxTimeoutRetries` times, because each try can already take minutes.
struct RetryPolicy {
    int maxRetries = 4;
    int baseDelayMs = 1000;
    int maxDelayMs = 20000;
    int maxTimeoutRetries = 1;
};
struct RetryNotice {
    int attempt = 0;      // the try that just failed (1 = first)
    int maxAttempts = 0;
    int delayMs = 0;      // wait before the next try
    int status = 0;       // HTTP status, 0 = transport failure
    std::string reason;   // short, no secrets
};
class RetryingHttpTransport final : public HttpTransport {
public:
    explicit RetryingHttpTransport(std::unique_ptr<HttpTransport> inner, RetryPolicy policy = {});
    HttpResponse send(const HttpRequest& request) override;
    // Called before each wait (UI: "retrying in 2 s"). Set once, before requests run.
    std::function<void(const RetryNotice&)> onRetry;
    // Returns true to give up at once (AI switched off, editor closing).
    std::function<bool()> shouldStop;
    // Replaceable wait (tests). Default sleeps in short slices so shouldStop is seen quickly.
    std::function<void(int ms)> sleepFn;

    HttpTransport& inner() { return *m_inner; }  // one plain try (connection test)

    static bool retryable(const HttpResponse& r);
    static bool isTimeout(const HttpResponse& r);
    static int delayFor(const RetryPolicy& p, int retryIndex);  // without jitter; retryIndex 0 = first retry

private:
    std::unique_ptr<HttpTransport> m_inner;
    RetryPolicy m_policy;
};

// Test double: records requests and replays queued responses (FIFO).
class MockHttpTransport final : public HttpTransport {
public:
    void enqueue(HttpResponse r) { m_responses.push_back(std::move(r)); }
    void enqueueJson(int status, const std::string& body) { enqueue({status, body, {}}); }
    HttpResponse send(const HttpRequest& request) override {
        m_requests.push_back(request);
        if (m_responses.empty()) return {0, {}, "mock: no queued response"};
        HttpResponse r = m_responses.front();
        m_responses.pop_front();
        return r;
    }
    const std::vector<HttpRequest>& requests() const { return m_requests; }

private:
    std::deque<HttpResponse> m_responses;
    std::vector<HttpRequest> m_requests;
};

} // namespace fledge::ai
