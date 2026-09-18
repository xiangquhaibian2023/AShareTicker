#include "stock_screener.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <set>

namespace {

std::vector<ashare::KLine> oscillatingLines() {
    std::vector<ashare::KLine> result;
    double previous = 10.0;
    for (int index = 0; index < 20; ++index) {
        double close = index % 2 == 0 ? previous * 1.004 : previous * 0.996;
        result.push_back({"2026-08-" + std::to_string(index + 1), previous,
                          close, previous * 1.012, previous * 0.988, 1'000'000.0});
        previous = close;
    }
    return result;
}

std::vector<ashare::KLine> trendingLines() {
    std::vector<ashare::KLine> result;
    double previous = 10.0;
    for (int index = 0; index < 20; ++index) {
        double close = previous * 1.018;
        result.push_back({"2026-08-" + std::to_string(index + 1), previous,
                          close, close * 1.002, previous * 0.999, 1'000'000.0});
        previous = close;
    }
    return result;
}

}  // namespace

int main() {
    using namespace ashare;
    Quote quote{"sh510300", "沪深300ETF", 4.05, 4.00, 4.01, 0.05, 1.25,
                200'000'000.0, 800'000'000.0, "20260825103000"};
    quote.high = 4.07;
    quote.low = 3.98;

    auto suitable = evaluateT0Suitability(quote, oscillatingLines());
    assert(suitable.score >= 70);
    assert(suitable.qualified);
    assert(suitable.opportunityDayPercent > 80.0);

    Quote illiquid = quote;
    illiquid.amount = 2'000'000.0;
    auto lowLiquidity = evaluateT0Suitability(illiquid, oscillatingLines());
    assert(lowLiquidity.score < suitable.score);
    assert(!lowLiquidity.qualified);

    auto trend = evaluateT0Suitability(quote, trendingLines());
    assert(trend.score < suitable.score);
    assert(trend.trendDeviationPercent > suitable.trendDeviationPercent);

    auto invalid = evaluateT0Suitability(quote, {{"2026-08-01", 0, 0, 0, 0, 0}});
    assert(invalid.score == 0);
    assert(!invalid.qualified);

    std::set<std::string> unique;
    for (const auto& symbol : liquidBalancedCandidates()) {
        assert(symbol.size() == 8);
        assert(symbol.rfind("sh", 0) == 0 || symbol.rfind("sz", 0) == 0);
        assert(unique.insert(symbol).second);
    }
    assert(unique.size() >= 50);

    std::cout << "Stock screener tests passed\n";
    return 0;
}
