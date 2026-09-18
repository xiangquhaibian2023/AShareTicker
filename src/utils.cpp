#include "utils.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace ashare {

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                   nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::string gbkToUtf8(const std::string& value) {
    if (value.empty()) return "";
    int wideSize = MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (wideSize <= 0) return value;
    std::wstring wide(wideSize, L'\0');
    MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()), wide.data(), wideSize);
    return wideToUtf8(wide);
}

std::string trim(std::string value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(),
                value.end());
    return value;
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream input(value);
    while (std::getline(input, part, delimiter)) parts.push_back(part);
    return parts;
}

double parseDouble(const std::string& value) {
    if (value.empty() || value == "-" || value == "--") return 0.0;
    try {
        return std::stod(value);
    } catch (...) {
        return 0.0;
    }
}

std::optional<TradingMinutePosition> tradingMinutePosition(const std::string& dateTime) {
    auto separator = dateTime.rfind(' ');
    if (separator == std::string::npos || separator + 5 >= dateTime.size()) return std::nullopt;
    const char* time = dateTime.c_str() + separator + 1;
    if (!std::isdigit(static_cast<unsigned char>(time[0])) ||
        !std::isdigit(static_cast<unsigned char>(time[1])) || time[2] != ':' ||
        !std::isdigit(static_cast<unsigned char>(time[3])) ||
        !std::isdigit(static_cast<unsigned char>(time[4]))) {
        return std::nullopt;
    }
    int timeOfDay = ((time[0] - '0') * 10 + (time[1] - '0')) * 60 +
                    (time[3] - '0') * 10 + (time[4] - '0');
    constexpr int morningOpen = 9 * 60 + 30;
    constexpr int morningClose = 11 * 60 + 30;
    constexpr int afternoonOpen = 13 * 60;
    constexpr int afternoonClose = 15 * 60;
    // 下午从 120 继续计数，因此 11:30 与 13:00 在图上共享午休分界位置。
    if (timeOfDay >= morningOpen && timeOfDay <= morningClose) {
        return TradingMinutePosition{timeOfDay - morningOpen, false};
    }
    if (timeOfDay >= afternoonOpen && timeOfDay <= afternoonClose) {
        return TradingMinutePosition{120 + timeOfDay - afternoonOpen, true};
    }
    return std::nullopt;
}

std::wstring formatNumber(double value, int precision) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

bool isFundLikeSymbol(const std::string& symbol) {
    return symbol.size() >= 3 && (symbol.rfind("sh5", 0) == 0 || symbol.rfind("sz1", 0) == 0);
}

const std::vector<std::string>& majorIndexSymbols() {
    static const std::vector<std::string> symbols = {
        "sh000001", "sz399001", "sz399006", "sh000688", "sh000300", "sh000016", "sh000905",
    };
    return symbols;
}

std::wstring formatLarge(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(2);
    if (std::fabs(value) >= 100000000.0) out << value / 100000000.0 << L"亿";
    else if (std::fabs(value) >= 10000.0) out << value / 10000.0 << L"万";
    else out << value;
    return out.str();
}

std::optional<std::string> normalizeSymbol(std::string input) {
    input = trim(input);
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    input.erase(std::remove_if(input.begin(), input.end(),
                               [](unsigned char c) { return c == '.' || c == '_'; }),
                input.end());
    auto isDigit = [](unsigned char c) { return std::isdigit(c) != 0; };
    // 常见指数别名在这里集中维护，最终都转换成 sh/sz + 6 位代码。
    static const std::map<std::string, std::string> aliases = {
        {"shcomp", "sh000001"}, {"sse", "sh000001"}, {"上证指数", "sh000001"},
        {"szcomp", "sz399001"}, {"深证成指", "sz399001"},
        {"chinext", "sz399006"}, {"cyb", "sz399006"}, {"创业板指", "sz399006"},
        {"star50", "sh000688"}, {"kc50", "sh000688"}, {"科创50", "sh000688"},
        {"hs300", "sh000300"}, {"沪深300", "sh000300"},
        {"sse50", "sh000016"}, {"上证50", "sh000016"},
        {"zz500", "sh000905"}, {"中证500", "sh000905"},
    };
    if (auto alias = aliases.find(input); alias != aliases.end()) return alias->second;
    if (input.size() == 8 && (input.rfind("sh", 0) == 0 || input.rfind("sz", 0) == 0)) {
        std::string code = input.substr(2);
        if (std::all_of(code.begin(), code.end(), isDigit)) return input;
    }
    if (input.size() != 6 || !std::all_of(input.begin(), input.end(), isDigit)) return std::nullopt;
    // 根据 A 股代码首位推断交易所；无法判断的代码直接拒绝。
    if (input[0] == '6' || input[0] == '5' || input[0] == '9') return "sh" + input;
    if (input[0] == '0' || input[0] == '1' || input[0] == '2' || input[0] == '3') return "sz" + input;
    return std::nullopt;
}

std::string currentDateToken() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char value[16]{};
    std::snprintf(value, sizeof(value), "%04u%02u%02u", time.wYear, time.wMonth, time.wDay);
    return value;
}

std::string eastmoneySecid(const std::string& symbol) {
    return (symbol.rfind("sh", 0) == 0 ? "1." : "0.") + symbol.substr(2);
}

std::filesystem::path applicationStorageDirectory() {
    std::wstring buffer(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::filesystem::current_path();
    buffer.resize(length);
    std::filesystem::path directory = std::filesystem::path(buffer).parent_path();
    // 从 build 或多配置输出目录运行时，向上回到项目根目录，确保数据位置稳定。
    if ((directory.filename() == L"Release" || directory.filename() == L"Debug" ||
         directory.filename() == L"RelWithDebInfo" || directory.filename() == L"MinSizeRel") &&
        directory.parent_path().filename() == L"build") {
        directory = directory.parent_path();
    }
    if (directory.filename() == L"build") directory = directory.parent_path();
    return directory;
}

void writeTextFileAtomically(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to open temporary data file.");
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) throw std::runtime_error("Unable to write temporary data file.");
    }
    // WRITE_THROUGH 尽量让替换落盘；失败时清理临时文件并保留原文件。
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("Unable to replace data file, error=" + std::to_string(error));
    }
}

Logger::Logger(std::filesystem::path directory) : directory_(std::move(directory)) {}

void Logger::write(const char* level, const std::string& message) noexcept {
    // 日志不能反向打断行情线程，因此整个写入过程采用 noexcept + 兜底捕获。
    try {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        char date[16]{};
        char timestamp[32]{};
        std::snprintf(date, sizeof(date), "%04u%02u%02u", time.wYear, time.wMonth, time.wDay);
        std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
                      time.wMilliseconds);
        std::lock_guard<std::mutex> lock(mutex_);
        std::filesystem::create_directories(directory_);
        std::ofstream output(directory_ / (std::string("ashare_") + date + ".log"),
                             std::ios::binary | std::ios::app);
        if (output) output << timestamp << " [" << level << "] " << message << '\n';
    } catch (...) {
    }
}

Logger& logger() {
    // 有意使用进程生命周期单例，避免静态析构顺序影响退出阶段日志。
    static Logger* instance = new Logger(applicationStorageDirectory() / L"logs");
    return *instance;
}

}  // namespace ashare
