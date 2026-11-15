#include <algorithm>
#include <format>

#include "fledge/ai/http.h"
#include "fledge/core/utility.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace fledge::ai {

#ifdef _WIN32
namespace {

class WinHttpTransport final : public HttpTransport {
public:
    WinHttpTransport() {
        m_session = WinHttpOpen(L"Fledge/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    ~WinHttpTransport() override {
        if (m_session) WinHttpCloseHandle(m_session);
    }

    HttpResponse send(const HttpRequest& req) override {
        HttpResponse res;
        if (!m_session) return fail("WinHttpOpen failed");
        std::wstring url = widen(req.url);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256], path[2048];
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return fail("invalid URL");
        bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        HINTERNET conn = WinHttpConnect(m_session, host, uc.nPort, 0);
        if (!conn) return fail("connect failed");
        HINTERNET r = WinHttpOpenRequest(conn, widen(req.method).c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!r) {
            WinHttpCloseHandle(conn);
            return fail("open request failed");
        }
        // resolve / connect fail fast; sending and waiting for the answer get the long limit (see HttpRequest::timeoutMs)
        WinHttpSetTimeouts(r, 15000, std::min(req.timeoutMs, 30000), req.timeoutMs, req.timeoutMs);
        std::wstring headers;
        for (auto& [k, v] : req.headers) headers += widen(k) + L": " + widen(v) + L"\r\n";
        BOOL ok = WinHttpSendRequest(r, headers.c_str(), static_cast<DWORD>(-1L), const_cast<char*>(req.body.data()),
                                     static_cast<DWORD>(req.body.size()), static_cast<DWORD>(req.body.size()), 0);
        if (ok) ok = WinHttpReceiveResponse(r, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            WinHttpCloseHandle(r);
            WinHttpCloseHandle(conn);
            return fail(describeTransportError(err));
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        res.status = static_cast<int>(status);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &read)) break;
            res.body.append(chunk.data(), read);
        }
        WinHttpCloseHandle(r);
        WinHttpCloseHandle(conn);
        return res;
    }

private:
    static HttpResponse fail(std::string e) { return {0, {}, std::move(e)}; }
    HINTERNET m_session = nullptr;
};

} // namespace

std::unique_ptr<HttpTransport> createDefaultHttpTransport() { return std::make_unique<WinHttpTransport>(); }
#else
namespace {
class NoHttp final : public HttpTransport {
public:
    HttpResponse send(const HttpRequest&) override { return {0, {}, "no HTTP transport on this platform yet (TODO: libcurl)"}; }
};
} // namespace
std::unique_ptr<HttpTransport> createDefaultHttpTransport() { return std::make_unique<NoHttp>(); }
#endif

} // namespace fledge::ai
