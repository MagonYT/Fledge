#include <algorithm>
#include <chrono>
#include <format>
#include <random>
#include <thread>

#include "fledge/ai/http.h"

namespace fledge::ai {

std::string describeTransportError(unsigned long e) {
    const char* what = nullptr;
    switch (e) {
        case 12002: what = "timed out waiting for the server (the model took too long to answer, or the connection stalled)"; break;
        case 12007: what = "the server name could not be found (check the URL and your internet connection)"; break;
        case 12029: what = "could not connect to the server (is it running, and is the port right?)"; break;
        case 12030: what = "the connection was closed before the answer arrived"; break;
        case 12152: what = "the server sent an answer that is not valid HTTP"; break;
        case 12175: what = "the secure connection failed (TLS / certificate problem)"; break;
        case 12017: what = "the request was cancelled"; break;
        default: break;
    }
    return what ? std::format("request failed: {} (WinHTTP error {})", what, e) : std::format("request failed (WinHTTP error {})", e);
}

RetryingHttpTransport::RetryingHttpTransport(std::unique_ptr<HttpTransport> inner, RetryPolicy policy)
    : m_inner(std::move(inner)), m_policy(policy) {}

bool RetryingHttpTransport::isTimeout(const HttpResponse& r) {
    return r.status == 0 && (r.error.find("12002") != std::string::npos || r.error.find("timed out") != std::string::npos);
}

bool RetryingHttpTransport::retryable(const HttpResponse& r) {
    if (r.status == 0) return r.error.find("invalid URL") == std::string::npos && r.error.find("mock:") == std::string::npos;
    return r.status == 408 || r.status == 409 || r.status == 425 || r.status == 429 || r.status >= 500;
}

int RetryingHttpTransport::delayFor(const RetryPolicy& p, int retryIndex) {
    long long d = static_cast<long long>(std::max(1, p.baseDelayMs)) << std::clamp(retryIndex, 0, 20);
    return static_cast<int>(std::min<long long>(d, std::max(p.baseDelayMs, p.maxDelayMs)));
}

HttpResponse RetryingHttpTransport::send(const HttpRequest& request) {
    static thread_local std::mt19937 rng{std::random_device{}()};
    const int maxAttempts = std::max(0, m_policy.maxRetries) + 1;
    int timeouts = 0;
    for (int attempt = 1;; ++attempt) {
        HttpResponse r = m_inner->send(request);
        if (!retryable(r) || attempt >= maxAttempts) return r;
        if (isTimeout(r) && ++timeouts > m_policy.maxTimeoutRetries) return r;
        if (shouldStop && shouldStop()) return r;
        int delay = delayFor(m_policy, attempt - 1);
        delay += static_cast<int>(rng() % static_cast<unsigned>(delay / 4 + 1));  // jitter: clients do not retry in step
        if (onRetry) {
            RetryNotice n;
            n.attempt = attempt;
            n.maxAttempts = maxAttempts;
            n.delayMs = delay;
            n.status = r.status;
            n.reason = r.status == 0 ? r.error : std::format("HTTP {}", r.status);
            onRetry(n);
        }
        if (sleepFn) {
            sleepFn(delay);
        } else {
            for (int left = delay; left > 0; left -= 100) {
                if (shouldStop && shouldStop()) return r;
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(left, 100)));
            }
        }
    }
}

} // namespace fledge::ai
