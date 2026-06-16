#include "director/keel_client.h"

#include "director/json.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace br::director {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// RAII for WinHTTP handles (closed in reverse-acquire order on scope exit).
struct WinHandle {
    HINTERNET h = nullptr;
    ~WinHandle() { if (h) WinHttpCloseHandle(h); }
};

// POST an already-built JSON `body` to the KEEL chat endpoint and parse the OpenAI
// envelope. Shared by keel_complete (text) and keel_complete_vision (text + image) —
// only the body differs. Never throws; any failure sets r.error and leaves r.ok=false.
KeelResponse keel_post(const std::string& host, int port, const std::string& body,
                       uint32_t timeout_ms) {
    KeelResponse r;

    WinHandle session, connect, request;
    session.h = WinHttpOpen(L"backrooms-director/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.error = "WinHttpOpen failed"; return r; }
    WinHttpSetTimeouts(session.h, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));

    const std::wstring whost = widen(host);
    connect.h = WinHttpConnect(session.h, whost.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!connect.h) { r.error = "WinHttpConnect failed (gle=" + std::to_string(GetLastError()) + ", host=" + host + ", port=" + std::to_string(port) + ")"; return r; }

    request.h = WinHttpOpenRequest(connect.h, L"POST", L"/v1/chat/completions", nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);  // plain HTTP (local)
    if (!request.h) { r.error = "WinHttpOpenRequest failed"; return r; }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        r.error = "WinHttpSendRequest failed (KEEL sidecar unreachable?)";
        return r;
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) { r.error = "WinHttpReceiveResponse failed"; return r; }

    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    r.http_status = static_cast<int>(status);

    std::string resp;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail) || avail == 0) break;
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buf.data(), avail, &read) || read == 0) break;
        resp.append(buf.data(), read);
    }

    if (status != 200) { r.error = "HTTP status " + std::to_string(status) + ": " + resp; return r; }

    json::Value root;
    std::string perr;
    if (!json::parse(resp, root, perr)) { r.error = "bad response JSON: " + perr; return r; }

    const json::Value* choices = root.find("choices");
    const json::Value* c0 = choices ? choices->at(0) : nullptr;
    const json::Value* msg = c0 ? c0->find("message") : nullptr;
    const json::Value* content = msg ? msg->find("content") : nullptr;
    if (!content || !content->is_string()) { r.error = "no choices[0].message.content in response"; return r; }
    r.content = content->str;

    if (const json::Value* keel = root.find("keel")) {
        if (const json::Value* t = keel->find("tier"); t && t->is_string()) r.tier = t->str;
        if (const json::Value* rt = keel->find("route"); rt && rt->is_string()) r.route = rt->str;
        if (const json::Value* co = keel->find("cost"); co && co->is_number()) r.cost = co->num;
    }
    r.ok = true;
    return r;
}

}  // namespace

KeelResponse keel_complete(const std::string& host, int port, const std::string& prompt,
                           uint32_t timeout_ms) {
    // A single user turn (plain text) + the local-tier routing flags.
    const std::string body =
        std::string("{\"messages\":[{\"role\":\"user\",\"content\":\"") + json::escape(prompt) +
        "\"}],\"sovereign\":true,\"kind\":\"scaffolding\",\"think\":false}";
    return keel_post(host, port, body, timeout_ms);
}

KeelResponse keel_complete_vision(const std::string& host, int port, const std::string& prompt,
                                  const std::string& image_base64, uint32_t timeout_ms) {
    // A single user turn whose content is an array of parts: the text prompt + an
    // OpenAI image_url part carrying the PNG as a base64 data URI. The base64 alphabet
    // (A-Z a-z 0-9 + / =) is JSON-safe, so only the prompt needs escaping.
    const std::string body =
        std::string("{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"") +
        json::escape(prompt) +
        "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," +
        image_base64 +
        "\"}}]}],\"sovereign\":true,\"kind\":\"scaffolding\",\"think\":false}";
    return keel_post(host, port, body, timeout_ms);
}

}  // namespace br::director
