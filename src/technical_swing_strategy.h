#pragma once

#include "trading_algorithm.h"

namespace ashare {

std::string serializeTechnicalSwingConfig(const TechnicalSwingConfig& config);
bool parseTechnicalSwingConfig(const std::string& text, TechnicalSwingConfig& config);

class TechnicalSwingStrategy final : public TradingAlgorithm {
public:
    explicit TechnicalSwingStrategy(TechnicalSwingConfig config = {});
    std::string name() const override { return "technical_swing"; }
    TechnicalSwingResult analyze(const std::vector<KLine>& daily,
                                 const std::string& symbol = {}) const;
    std::vector<TechnicalSwingResult> analyzeHistory(const std::vector<KLine>& daily,
                                                   const std::string& symbol = {}) const;
    // 适配已有接口，始终返回 None；仅附加因子结果，忽略持仓成本。
    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute,
                           const Holding* holding = nullptr) const override;
private:
    TechnicalSwingConfig config_;
};

// 包装已有算法，保持其最终方向、数量、消息等字段不变。
std::unique_ptr<TradingAlgorithm> withTechnicalSwing(
    std::unique_ptr<TradingAlgorithm> algorithm, const TechnicalSwingConfig& config);

}  // namespace ashare
