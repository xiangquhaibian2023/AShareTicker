#pragma once

#include <memory>
#include <string>

namespace ashare {

// 基于 WinHTTP 的同步 GET 客户端，封装超时、TLS、重试和响应解压细节。
class WinHttpClient {
public:
    WinHttpClient();
    ~WinHttpClient();
    WinHttpClient(const WinHttpClient&) = delete;
    WinHttpClient& operator=(const WinHttpClient&) = delete;
    WinHttpClient(WinHttpClient&&) noexcept;
    WinHttpClient& operator=(WinHttpClient&&) noexcept;

    // 请求成功返回响应体；所有重试均失败时抛出 std::runtime_error。
    std::string get(const std::string& url) const;

private:
    // PImpl 用于避免在公共头文件中暴露 WinHTTP 句柄类型。
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ashare
