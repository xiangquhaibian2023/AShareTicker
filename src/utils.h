#pragma once

#include "domain.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ashare {

// 编码、字符串与数值辅助函数。
std::wstring utf8ToWide(const std::string& value);
std::string wideToUtf8(const std::wstring& value);
std::string gbkToUtf8(const std::string& value);
std::string trim(std::string value);
std::vector<std::string> split(const std::string& value, char delimiter);
double parseDouble(const std::string& value);
// 将 09:30-11:30、13:00-15:00 映射到连续的 0-240 分钟坐标。
std::optional<TradingMinutePosition> tradingMinutePosition(const std::string& dateTime);
std::wstring formatNumber(double value, int precision = 2);
std::wstring formatLarge(double value);
bool isFundLikeSymbol(const std::string& symbol);
const std::vector<std::string>& majorIndexSymbols();
std::optional<std::string> normalizeSymbol(std::string input);
std::string currentDateToken();
std::string eastmoneySecid(const std::string& symbol);

// 数据目录位于 exe 同级；写文件采用临时文件替换，避免异常中断损坏原文件。
std::filesystem::path applicationStorageDirectory();
void writeTextFileAtomically(const std::filesystem::path& path, const std::string& content);

// 线程安全的按日文件日志器，日志写入失败不会影响行情主流程。
class Logger {
public:
    explicit Logger(std::filesystem::path directory);
    void write(const char* level, const std::string& message) noexcept;

private:
    std::filesystem::path directory_;
    std::mutex mutex_;
};

Logger& logger();

}  // namespace ashare
