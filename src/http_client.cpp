#include "http_client.h"

#include "utils.h"

#include <windows.h>
#include <winhttp.h>

#include <stdexcept>
#include <utility>

namespace ashare {
namespace {

// WinHTTP 建连只需要主机和请求路径，因此先将完整 URL 拆成这三部分。
struct UrlParts {
    std::wstring host;
    std::wstring path;
    bool https = false;
};

UrlParts parseUrl(const std::string& url) {
    UrlParts parts;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        parts.https = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else {
        throw std::runtime_error("Only http/https URL is supported.");
    }
    auto slash = rest.find('/');
    parts.host = utf8ToWide(slash == std::string::npos ? rest : rest.substr(0, slash));
    parts.path = utf8ToWide(slash == std::string::npos ? "/" : rest.substr(slash));
    return parts;
}

// HINTERNET 的 RAII 包装，确保异常路径也能关闭 session/connect/request 句柄。
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    ~WinHttpHandle() { reset(); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.release()) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }
    void reset(HINTERNET handle = nullptr) {
        if (handle_) WinHttpCloseHandle(handle_);
        handle_ = handle;
    }
    HINTERNET release() {
        HINTERNET handle = handle_;
        handle_ = nullptr;
        return handle;
    }

private:
    HINTERNET handle_ = nullptr;
};

std::runtime_error winHttpError(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed, error=" + std::to_string(GetLastError()));
}

}  // namespace

struct WinHttpClient::Impl {
    std::string get(const std::string& url) const {
        std::string lastError = "HTTP request failed.";
        // 首次失败后丢弃共享 session 再试一次，可恢复代理切换或失效连接。
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                return getOnce(url);
            } catch (const std::exception& ex) {
                lastError = ex.what();
                if (attempt < 1) {
                    session.reset();
                    Sleep(200 * (attempt + 1));
                }
            }
        }
        logger().write("WARN", "HTTP GET failed url=" + url + " error=" + lastError);
        throw std::runtime_error(lastError);
    }

    HINTERNET getSession() const {
        if (!session) {
            // session 在请求间复用；四个超时依次对应解析、连接、发送和接收。
            session.reset(WinHttpOpen(L"AShareTickerClient/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session) throw winHttpError("WinHttpOpen");
            if (!WinHttpSetTimeouts(session.get(), 4000, 4000, 6000, 10000)) {
                session.reset();
                throw winHttpError("WinHttpSetTimeouts");
            }
        }
        return session.get();
    }

    std::string getOnce(const std::string& url) const {
        UrlParts parts = parseUrl(url);
        // connect 和 request 都是单次请求资源，函数退出时由 WinHttpHandle 自动释放。
        WinHttpHandle connect(WinHttpConnect(getSession(), parts.host.c_str(),
                                             parts.https ? INTERNET_DEFAULT_HTTPS_PORT
                                                         : INTERNET_DEFAULT_HTTP_PORT,
                                             0));
        if (!connect) throw winHttpError("WinHttpConnect");
        WinHttpHandle request(WinHttpOpenRequest(connect.get(), L"GET", parts.path.c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 parts.https ? WINHTTP_FLAG_SECURE : 0));
        if (!request) throw winHttpError("WinHttpOpenRequest");
        std::wstring headers = L"Accept: */*\r\nUser-Agent: AShareTickerClient/1.0\r\n";
        if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            throw winHttpError("WinHttpSendRequest");
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) throw winHttpError("WinHttpReceiveResponse");
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            throw winHttpError("WinHttpQueryHeaders");
        }
        if (statusCode < 200 || statusCode >= 300) {
            throw std::runtime_error("HTTP status=" + std::to_string(statusCode));
        }

        // 公开接口响应不应达到该体积；上限可防止异常响应耗尽内存。
        constexpr size_t maxResponseBytes = 16 * 1024 * 1024;
        std::string body;
        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            if (contentLength > maxResponseBytes) throw std::runtime_error("HTTP response is too large.");
            body.reserve(contentLength);
        }
        // WinHTTP 可能分块返回响应，循环读取直到可用字节为 0。
        while (true) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                throw winHttpError("WinHttpQueryDataAvailable");
            }
            if (available == 0) break;
            if (available > maxResponseBytes - body.size()) {
                throw std::runtime_error("HTTP response is too large.");
            }
            std::string buffer(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), available, &read)) {
                throw winHttpError("WinHttpReadData");
            }
            buffer.resize(read);
            body += buffer;
        }
        return body;
    }

    // get() 为逻辑只读操作，但允许延迟创建和重建底层会话。
    mutable WinHttpHandle session;
};

WinHttpClient::WinHttpClient() : impl_(std::make_unique<Impl>()) {}
WinHttpClient::~WinHttpClient() = default;
WinHttpClient::WinHttpClient(WinHttpClient&&) noexcept = default;
WinHttpClient& WinHttpClient::operator=(WinHttpClient&&) noexcept = default;

std::string WinHttpClient::get(const std::string& url) const {
    return impl_->get(url);
}

}  // namespace ashare
