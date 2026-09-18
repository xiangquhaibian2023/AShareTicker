#include "position_advisor.h"

#include <cassert>
#include <iostream>

using namespace ashare;

int main() {
    Holding holding;
    holding.symbol = "sh513050";
    holding.quantity = 50000;
    holding.cost = 1.10;
    Quote quote;
    quote.symbol = holding.symbol;
    quote.name = "ETF";
    quote.price = 1.20;
    quote.volume = 2'000'000;
    std::vector<KLine> daily;
    for (int index = 0; index < 50; ++index) {
        double close = 1.0 + index * 0.004;
        daily.push_back({"2026", close - 0.002, close, close + 0.01,
                         close - 0.01, 1'000'000.0 + index * 10'000.0});
    }
    PositionAdvice advice = evaluatePositionAdvice(holding, quote, daily,
                                                    200000.0, 30000.0, 20.0);
    assert(advice.action != PositionAction::DataInsufficient);
    assert(advice.ma5 > advice.ma10);
    assert(advice.ma10 > advice.ma20);
    assert(advice.addPriceLow > 0.0);
    assert(advice.addPriceHigh <= quote.price);
    assert(advice.reducePriceHigh >= advice.reducePriceLow);
    assert(advice.reducePriceLow >= quote.price);
    assert(advice.addQuantity % 100 == 0);
    assert(advice.reduceQuantity % 100 == 0);

    PositionAdvice concentrated = evaluatePositionAdvice(holding, quote, daily,
                                                          100000.0, 30000.0, 55.0);
    assert(concentrated.addQuantity == 0);
    std::cout << "Position advisor tests passed\n";
    return 0;
}
