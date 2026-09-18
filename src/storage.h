#pragma once

#include "domain.h"
#include "signal_history.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ashare {

// 自选代码集合，每行保存一个标准化证券代码。
class FavoriteStore {
public:
    explicit FavoriteStore(std::filesystem::path path);
    std::set<std::string> load() const;
    void save(const std::set<std::string>& symbols) const;

private:
    std::filesystem::path path_;
};

// 持仓 TSV 存储，同时保存当天成交汇总，供重启后继续计算当日盈亏。
class HoldingStore {
public:
    explicit HoldingStore(std::filesystem::path path);
    std::map<std::string, Holding> load() const;
    void save(const std::map<std::string, Holding>& holdings) const;

private:
    std::filesystem::path path_;
};

// 分笔成交账本。持仓和策略分别使用独立文件，但共享同一稳定格式。
class TradeRecordStore {
public:
    explicit TradeRecordStore(std::filesystem::path path);
    std::vector<TradeRecord> load() const;
    void save(const std::vector<TradeRecord>& records) const;

private:
    std::filesystem::path path_;
};

class AccountFundsStore {
public:
    explicit AccountFundsStore(std::filesystem::path path);
    AccountFunds load() const;
    void save(const AccountFunds& funds) const;

private:
    std::filesystem::path path_;
};

// 通用证券代码集合存储，当前用于策略观察标的。
class SymbolSetStore {
public:
    explicit SymbolSetStore(std::filesystem::path path);
    std::set<std::string> load() const;
    void save(const std::set<std::string>& symbols) const;

private:
    std::filesystem::path path_;
};

// 策略列表和当前选中策略分别存储，避免改列表时覆盖用户选择。
class StrategyConfigStore {
public:
    StrategyConfigStore(std::filesystem::path configsPath, std::filesystem::path activePath);
    std::vector<StrategyConfig> load() const;
    void save(const std::vector<StrategyConfig>& configs) const;
    std::string loadActive() const;
    void saveActive(const std::string& id) const;

private:
    std::filesystem::path configsPath_;
    std::filesystem::path activePath_;
};

// 当天实盘策略信号存储。加载其他交易日时返回空集合，旧记录不会混入今天。
class StrategySignalStore {
public:
    explicit StrategySignalStore(std::filesystem::path path);
    std::vector<StrategySignalRecord> load(const std::string& dateToken) const;
    void save(const std::string& dateToken,
              const std::vector<StrategySignalRecord>& records) const;

private:
    std::filesystem::path path_;
};

}  // namespace ashare
