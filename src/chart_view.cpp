#include "chart_view.h"

#include "app_state.h"
#include "trade_ledger.h"
#include "ui_theme.h"
#include "utils.h"
#include "technical_swing_strategy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ashare {

namespace {

// 行情源可能在日期后附带时间，日 K 横轴只显示 YYYY-MM-DD。
std::wstring formatAxisDate(const std::string& value) {
    if (value.empty()) {
        return L"--";
    }
    size_t end = value.find(' ');
    if (end == std::string::npos) {
        end = value.size();
    }
    return utf8ToWide(value.substr(0, std::min<size_t>(10, end)));
}

} // namespace

LRESULT CALLBACK KLineProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC paintDc = BeginPaint(hwnd, &ps);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            EndPaint(hwnd, &ps);
            return 0;
        }
        // 先在内存位图中完成整张图，再一次性复制到窗口，避免定时刷新闪烁。
        HDC dc = CreateCompatibleDC(paintDc);
        HBITMAP bitmap = CreateCompatibleBitmap(paintDc, width, height);
        HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(dc, bitmap));
        // 统一收尾，保证每个提前返回分支都释放 GDI 资源并结束绘制。
        auto present = [&]() {
            BitBlt(paintDc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(hwnd, &ps);
        };
        HWND parent = GetParent(hwnd);
        AppState* state = app(parent);
        if (state) {
            // 每次绘制都按当前算法和周期重建命中区域，窗口缩放后悬停位置仍然准确。
            state->chartTradeMarkers.clear();
            state->chartCandleMarkers.clear();
        }
        ThemePalette palette = themePalette();
        HBRUSH backgroundBrush = CreateSolidBrush(palette.surface);
        FillRect(dc, &rect, backgroundBrush);
        DeleteObject(backgroundBrush);
        HFONT oldFont = nullptr;
        if (state && state->font) {
            oldFont = static_cast<HFONT>(SelectObject(dc, state->font));
        }

        if (!state || state->kLines.empty()) {
            if (state) {
                state->hoveredTradeMarker.reset();
                state->hoveredCandleMarker.reset();
            }
            SetTextColor(dc, palette.muted);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"选择本页标的后显示走势", -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            present();
            return 0;
        }

        // 为标题、价格轴和时间轴预留边距，plot 仅表示实际行情绘制区域。
        RECT plot = rect;
        plot.left += state->currentKlt == 1 ? 82 : 64;
        plot.right -= state->currentKlt == 1 ? 92 : 18;
        plot.top += state->currentKlt == 1 ? 40 : 30;
        plot.bottom -= 42;

        double minLow = std::numeric_limits<double>::max();
        double maxHigh = std::numeric_limits<double>::lowest();
        double minClose = std::numeric_limits<double>::max();
        double maxClose = std::numeric_limits<double>::lowest();
        for (const auto& line : state->kLines) {
            minLow = std::min(minLow, line.low);
            maxHigh = std::max(maxHigh, line.high);
            minClose = std::min(minClose, line.close);
            maxClose = std::max(maxClose, line.close);
        }
        // 分时图只关心成交价；日 K 必须包含最高价和最低价影线。
        double plotMin = state->currentKlt == 1 ? minClose : minLow;
        double plotMax = state->currentKlt == 1 ? maxClose : maxHigh;
        double referencePrice = 0.0;
        if (state->currentKlt == 1) {
            auto quote = state->quoteCache.find(state->currentSymbol);
            if (quote != state->quoteCache.end() && quote->second.previousClose > 0.0) {
                referencePrice = quote->second.previousClose;
            } else if (!state->kLines.empty()) {
                referencePrice = state->kLines.front().open > 0.0
                                     ? state->kLines.front().open
                                     : state->kLines.front().close;
            }
            // 分时纵轴以昨收为中心对称展开，涨跌幅在视觉上保持同一比例。
            double maximumDeviation = std::max(std::fabs(plotMax - referencePrice),
                                               std::fabs(plotMin - referencePrice));
            maximumDeviation = std::max(maximumDeviation,
                                        std::max(0.0001, referencePrice * 0.003));
            maximumDeviation *= 1.08;
            plotMin = referencePrice - maximumDeviation;
            plotMax = referencePrice + maximumDeviation;
        } else {
            double padding = std::max(0.0001, (plotMax - plotMin) * 0.04);
            plotMin -= padding;
            plotMax += padding;
        }
        double range = std::max(0.0001, plotMax - plotMin);
        auto yOf = [&](double price) {
            return plot.bottom - static_cast<int>((price - plotMin) / range * (plot.bottom - plot.top));
        };

        HPEN gridPen = CreatePen(PS_SOLID, 1, palette.grid);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, gridPen));
        SetBkMode(dc, TRANSPARENT);
        int pricePrecision = isFundLikeSymbol(state->currentSymbol) ? 3 : 2;
        HFONT axisPreviousFont = state->chartAxisFont
                                     ? static_cast<HFONT>(SelectObject(dc, state->chartAxisFont))
                                     : nullptr;
        for (int i = 0; i <= 4; ++i) {
            int y = plot.top + (plot.bottom - plot.top) * i / 4;
            MoveToEx(dc, plot.left, y, nullptr);
            LineTo(dc, plot.right, y);
            if (state->currentKlt == 1) {
                double price = plotMax - range * static_cast<double>(i) / 4.0;
                double percent = referencePrice > 0.0
                                     ? (price / referencePrice - 1.0) * 100.0
                                     : 0.0;
                RECT priceRect{rect.left + 3, y - 10, plot.left - 6, y + 10};
                RECT percentRect{plot.right + 6, y - 10, rect.right - 3, y + 10};
                MoveToEx(dc, plot.left - 5, y, nullptr);
                LineTo(dc, plot.left, y);
                MoveToEx(dc, plot.right, y, nullptr);
                LineTo(dc, plot.right + 5, y);
                SetTextColor(dc, palette.text);
                std::wstring priceText = formatNumber(price, pricePrecision);
                DrawTextW(dc, priceText.c_str(), -1, &priceRect,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(dc, percent > 0.0001 ? palette.up :
                                 percent < -0.0001 ? palette.down : palette.muted);
                std::wstring percentText = (percent > 0.0001 ? L"+" : L"") +
                                           formatNumber(percent, 2) + L"%";
                DrawTextW(dc, percentText.c_str(), -1, &percentRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        if (axisPreviousFont) {
            SelectObject(dc, axisPreviousFont);
        }
        SelectObject(dc, oldPen);
        DeleteObject(gridPen);

        int count = static_cast<int>(state->kLines.size());
        int plotWidth = static_cast<int>(plot.right - plot.left);
        if (state->currentKlt == 1) {
            // 午休分界单独画虚线；上午和下午折线分开，避免跨午休连接。
            int sessionDividerX = plot.left + (plotWidth - 1) / 2;
            HPEN sessionPen = CreatePen(PS_DOT, 1, palette.grid);
            HPEN previousPen = static_cast<HPEN>(SelectObject(dc, sessionPen));
            MoveToEx(dc, sessionDividerX, plot.top, nullptr);
            LineTo(dc, sessionDividerX, plot.bottom);
            SelectObject(dc, previousPen);
            DeleteObject(sessionPen);

            int referenceY = yOf(referencePrice);
            HPEN referencePen = CreatePen(PS_DOT, 1, palette.muted);
            previousPen = static_cast<HPEN>(SelectObject(dc, referencePen));
            MoveToEx(dc, plot.left, referenceY, nullptr);
            LineTo(dc, plot.right, referenceY);
            SelectObject(dc, previousPen);
            DeleteObject(referencePen);

            std::vector<POINT> morningPoints;
            std::vector<POINT> afternoonPoints;
            std::optional<POINT> latestPoint;
            for (int i = 0; i < count; ++i) {
                const auto& line = state->kLines[static_cast<size_t>(i)];
                auto position = tradingMinutePosition(line.date);
                if (!position) {
                    continue;
                }
                // 使用固定 240 分钟坐标，未发生的下午时段自然保持留白。
                int x = plot.left + position->minute * (plotWidth - 1) / 240;
                POINT point{x, yOf(line.close)};
                (position->afternoon ? afternoonPoints : morningPoints).push_back(point);
                latestPoint = point;
            }

            // 行情源通常以上午最后一分钟（11:29）的成交价代表上午收盘。
            // 午盘结束后把该价格延伸到 11:30；只补显示端点，不修改算法输入。
            bool morningSessionComplete = !afternoonPoints.empty();
            if (!morningSessionComplete && state->dataMode == MarketDataMode::Replay &&
                state->replay.ready()) {
                morningSessionComplete = state->replay.currentTime() >= "11:30";
            }
            if (!morningSessionComplete && state->dataMode == MarketDataMode::Live) {
                SYSTEMTIME now{};
                GetLocalTime(&now);
                morningSessionComplete = now.wHour * 60 + now.wMinute >= 11 * 60 + 30;
            }
            if (morningSessionComplete && !morningPoints.empty() &&
                morningPoints.back().x < sessionDividerX) {
                morningPoints.push_back(POINT{sessionDividerX, morningPoints.back().y});
            }

            // 部分实盘源下午首根为 13:01。用首根下午成交价补到午休分界，
            // 只修复图形起点，不向策略分钟数据注入虚构成交。
            if (!afternoonPoints.empty() &&
                afternoonPoints.front().x > sessionDividerX) {
                afternoonPoints.insert(
                    afternoonPoints.begin(),
                    POINT{sessionDividerX, afternoonPoints.front().y});
            }

            HPEN linePen = CreatePen(PS_SOLID, 2, palette.accent);
            previousPen = static_cast<HPEN>(SelectObject(dc, linePen));
            if (morningPoints.size() > 1) {
                Polyline(dc, morningPoints.data(), static_cast<int>(morningPoints.size()));
            }
            if (afternoonPoints.size() > 1) {
                Polyline(dc, afternoonPoints.data(), static_cast<int>(afternoonPoints.size()));
            }
            // 用户需要直接观察午后跳空：用同一条折线连接上午收盘与下午开盘，
            // 两个端点共享午休分界 X 坐标，因此价格变化显示为垂直直线。
            if (!morningPoints.empty() && !afternoonPoints.empty()) {
                MoveToEx(dc, morningPoints.back().x, morningPoints.back().y, nullptr);
                LineTo(dc, afternoonPoints.front().x, afternoonPoints.front().y);
            }
            SelectObject(dc, previousPen);
            DeleteObject(linePen);

            if (latestPoint) {
                HBRUSH markerBrush = CreateSolidBrush(palette.accent);
                HPEN markerPen = CreatePen(PS_SOLID, 1, palette.surface);
                HBRUSH previousBrush = static_cast<HBRUSH>(SelectObject(dc, markerBrush));
                previousPen = static_cast<HPEN>(SelectObject(dc, markerPen));
                Ellipse(dc, latestPoint->x - 4, latestPoint->y - 4, latestPoint->x + 5, latestPoint->y + 5);
                SelectObject(dc, previousBrush);
                SelectObject(dc, previousPen);
                DeleteObject(markerBrush);
                DeleteObject(markerPen);

                // 在最新价对应高度增加左右标签，价格和涨跌幅无需对照刻度推算。
                double latestPrice = state->kLines.back().close;
                double latestPercent = referencePrice > 0.0
                                           ? (latestPrice / referencePrice - 1.0) * 100.0
                                           : 0.0;
                int tagY = std::clamp(latestPoint->y,
                                      static_cast<LONG>(plot.top + 13),
                                      static_cast<LONG>(plot.bottom - 13));
                COLORREF tagColor = latestPercent > 0.0001 ? palette.up :
                                    latestPercent < -0.0001 ? palette.down : palette.accent;
                auto drawQuoteTag = [&](RECT tagRect, const std::wstring& text, UINT align) {
                    HBRUSH tagBrush = CreateSolidBrush(palette.surfaceAlt);
                    HPEN tagPen = CreatePen(PS_SOLID, 1, tagColor);
                    HGDIOBJ tagPreviousBrush = SelectObject(dc, tagBrush);
                    HGDIOBJ tagPreviousPen = SelectObject(dc, tagPen);
                    RoundRect(dc, tagRect.left, tagRect.top, tagRect.right, tagRect.bottom, 6, 6);
                    SelectObject(dc, tagPreviousBrush);
                    SelectObject(dc, tagPreviousPen);
                    DeleteObject(tagBrush);
                    DeleteObject(tagPen);
                    HFONT tagPreviousFont = state->chartAxisFont
                                                ? static_cast<HFONT>(SelectObject(dc, state->chartAxisFont))
                                                : nullptr;
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, tagColor);
                    InflateRect(&tagRect, -5, 0);
                    DrawTextW(dc, text.c_str(), -1, &tagRect,
                              align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (tagPreviousFont) {
                        SelectObject(dc, tagPreviousFont);
                    }
                };
                drawQuoteTag(RECT{rect.left + 4, tagY - 12, plot.left - 8, tagY + 13},
                             formatNumber(latestPrice, pricePrecision), DT_RIGHT);
                std::wstring percentTag =
                    std::wstring(latestPercent > 0.0001 ? L"+" : L"") +
                    formatNumber(latestPercent, 2) + L"%";
                drawQuoteTag(RECT{plot.right + 8, tagY - 12, rect.right - 4, tagY + 13},
                             percentTag, DT_LEFT);
            }
        } else {
            // 日 K 根据可用宽度计算实体宽度，至少保留 1 像素间距。
            int barSpace = std::max(4, plotWidth / std::max(1, count));
            int bodyWidth = std::max(3, barSpace - 2);
            HPEN redPen = CreatePen(PS_SOLID, 1, palette.up);
            HPEN greenPen = CreatePen(PS_SOLID, 1, palette.down);
            HBRUSH redBrush = CreateSolidBrush(palette.up);
            HBRUSH greenBrush = CreateSolidBrush(palette.down);

            for (int i = 0; i < count; ++i) {
                const auto& line = state->kLines[static_cast<size_t>(i)];
                bool up = line.close >= line.open;
                int x = plot.left + i * barSpace + barSpace / 2;
                SelectObject(dc, up ? redPen : greenPen);
                MoveToEx(dc, x, yOf(line.high), nullptr);
                LineTo(dc, x, yOf(line.low));
                RECT body{};
                body.left = x - bodyWidth / 2;
                body.right = x + bodyWidth / 2;
                body.top = yOf(std::max(line.open, line.close));
                body.bottom = yOf(std::min(line.open, line.close));
                // 开收盘相同时仍画出最小实体，避免十字线在高 DPI 下消失。
                if (body.bottom <= body.top) {
                    body.bottom = body.top + 2;
                }
                FillRect(dc, &body, up ? redBrush : greenBrush);

                ChartCandleMarker marker;
                // 整个蜡烛列均可悬停，避免细影线在高 DPI 屏幕上难以命中。
                marker.bounds = RECT{x - std::max(5, barSpace / 2), plot.top,
                                     x + std::max(6, barSpace / 2 + 1), plot.bottom};
                marker.anchor = POINT{x, (yOf(line.high) + yOf(line.low)) / 2};
                marker.lineIndex = static_cast<size_t>(i);
                state->chartCandleMarkers.push_back(marker);
            }

            DeleteObject(redPen);
            DeleteObject(greenPen);
            DeleteObject(redBrush);
            DeleteObject(greenBrush);
        }

        // 分钟线和日 K 统一读取分笔成交；回放模式则读取本次回放的模拟成交。
        {
            std::vector<TradeRecord> trades;
            const std::string chartDate = state->dataMode == MarketDataMode::Replay &&
                                                  state->replay.ready()
                                              ? tradeDateToken(state->replay.date())
                                              : currentDateToken();
            if (state->dataMode == MarketDataMode::Replay) {
                for (const auto& trade : state->strategySimulator.trades()) {
                    if (trade.strategyId == state->activeStrategyId &&
                        trade.symbol == state->currentSymbol) {
                        TradeRecord record;
                        record.id = "replay-" + trade.time + "-" +
                                    std::to_string(static_cast<int>(trade.side));
                        record.strategyId = trade.strategyId;
                        record.symbol = trade.symbol;
                        record.time = trade.time;
                        record.side = trade.side;
                        record.quantity = trade.quantity;
                        record.price = trade.executionPrice;
                        record.commission = trade.fees;
                        record.source = TradeSource::StrategySimulation;
                        trades.push_back(std::move(record));
                    }
                }
            } else if (state->detailView == DetailView::Holdings) {
                trades = tradesFor(state->holdingTrades, state->currentSymbol, chartDate);
            } else if (state->detailView == DetailView::Strategy) {
                trades = effectiveStrategyTrades(state->strategyTrades,
                                                  state->activeStrategyId,
                                                  state->currentSymbol, chartDate);
            }
            std::map<std::string, int> dailyTotals;
            std::map<std::string, int> dailyDrawn;
            if (state->currentKlt != 1) {
                for (const auto& trade : trades) {
                    dailyTotals[trade.time.substr(0, std::min<size_t>(10, trade.time.size()))]++;
                }
            }

            for (const auto& trade : trades) {
                int markerX = 0;
                if (state->currentKlt == 1) {
                    auto position = tradingMinutePosition(trade.time);
                    if (!position) {
                        continue;
                    }
                    markerX = plot.left + position->minute * (plotWidth - 1) / 240;
                } else {
                    std::string tradeDate = trade.time.substr(0, std::min<size_t>(10, trade.time.size()));
                    auto line = std::find_if(state->kLines.begin(), state->kLines.end(),
                                             [&](const KLine& item) {
                                                 return item.date.size() >= 10 &&
                                                        item.date.compare(0, 10, tradeDate) == 0;
                                             });
                    if (line == state->kLines.end()) {
                        continue;
                    }
                    int lineIndex = static_cast<int>(std::distance(state->kLines.begin(), line));
                    int barSpace = std::max(4, plotWidth / std::max(1, count));
                    markerX = plot.left + lineIndex * barSpace + barSpace / 2;
                    int order = dailyDrawn[tradeDate]++;
                    int total = dailyTotals[tradeDate];
                    markerX += (order * 2 - total + 1) * 4;
                }
                markerX = std::clamp(markerX, static_cast<int>(plot.left + 7),
                                     static_cast<int>(plot.right - 7));
                int markerY = std::clamp<int>(static_cast<int>(yOf(trade.price)),
                                              static_cast<int>(plot.top + 7),
                                              static_cast<int>(plot.bottom - 7));
                bool buy = trade.side == SignalType::Buy;
                POINT triangle[3] = {
                    POINT{markerX, markerY + (buy ? -7 : 7)},
                    POINT{markerX - 6, markerY + (buy ? 5 : -5)},
                    POINT{markerX + 6, markerY + (buy ? 5 : -5)},
                };
                COLORREF markerColor = buy ? palette.up : palette.down;
                HBRUSH markerBrush = CreateSolidBrush(markerColor);
                HPEN markerPen = CreatePen(PS_SOLID, 1, palette.text);
                HGDIOBJ previousBrush = SelectObject(dc, markerBrush);
                HGDIOBJ previousPen = SelectObject(dc, markerPen);
                Polygon(dc, triangle, 3);
                SelectObject(dc, previousBrush);
                SelectObject(dc, previousPen);
                DeleteObject(markerBrush);
                DeleteObject(markerPen);

                ChartTradeMarker marker;
                marker.bounds = RECT{markerX - 10, markerY - 10, markerX + 11, markerY + 11};
                marker.anchor = POINT{markerX, markerY};
                marker.trade = trade;
                state->chartTradeMarkers.push_back(std::move(marker));
            }
        }
        // 因子叠加复用现有日K坐标和行情缓存，BUY/SELL仅为技术候选标记。
        auto activeConfig = std::find_if(state->strategies.begin(), state->strategies.end(),
            [&](const StrategyConfig& c) { return c.id == state->activeStrategyId; });
        if (state->currentKlt != 1 && activeConfig != state->strategies.end() &&
            activeConfig->technicalSwing.enableTechnicalSwingStrategy && activeConfig->technicalSwing.showChartOverlay) {
            std::vector<KLine> daily = state->kLines;
            if (state->dataMode == MarketDataMode::Replay) {
                daily = state->replay.dailyLines(state->currentSymbol, 250);
            } else {
                std::lock_guard<std::mutex> lock(state->strategyHistoryCache->mutex);
                auto cached = state->strategyHistoryCache->entries.find(state->currentSymbol);
                if (cached != state->strategyHistoryCache->entries.end() && cached->second.daily.size() > daily.size())
                    daily = cached->second.daily;
            }
            // 图表当前OHLC优先；按日期匹配，禁止把缓存未来行画到旧图。
            std::map<std::string, KLine> byDate;
            for (const auto& b : daily) if (b.date <= state->kLines.back().date) byDate[b.date] = b;
            for (const auto& b : state->kLines) byDate[b.date] = b;
            daily.clear();
            for (const auto& [date, b] : byDate) daily.push_back(b);
            auto history = TechnicalSwingStrategy(activeConfig->technicalSwing).analyzeHistory(daily, state->currentSymbol);
            std::map<std::string, size_t> indices;
            for (size_t i = 0; i < daily.size(); ++i) indices[daily[i].date] = i;
            int savedDc = SaveDC(dc);
            IntersectClipRect(dc, plot.left, plot.top, plot.right, plot.bottom);
            int spacing = std::max(4, plotWidth / std::max(1, count));
            const COLORREF colors[] = {palette.accent, palette.up, palette.down};
            for (size_t k = 0; k < 3; ++k) {
                size_t period = activeConfig->technicalSwing.maPeriods[k];
                HPEN pen = CreatePen(PS_SOLID, 1, colors[k]);
                HGDIOBJ previous = SelectObject(dc, pen);
                bool started = false;
                for (int i = 0; i < count; ++i) {
                    size_t t = indices.at(state->kLines[i].date);
                    if (period == 0 || t + 1 < period) continue;
                    double sum = 0;
                    for (size_t j = t + 1 - period; j <= t; ++j) sum += daily[j].close;
                    int x = plot.left + i * spacing + spacing / 2, y = yOf(sum / period);
                    if (!started) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
                    started = true;
                }
                SelectObject(dc, previous); DeleteObject(pen);
            }
            if (!history.empty()) {
                auto drawLevel = [&](std::optional<double> level, const wchar_t* label, COLORREF color) {
                    if (!level) return;
                    HPEN pen = CreatePen(PS_DOT, 1, color);
                    HGDIOBJ previous = SelectObject(dc, pen);
                    int y = yOf(*level);
                    MoveToEx(dc, plot.left, y, nullptr); LineTo(dc, plot.right, y);
                    SetTextColor(dc, color); TextOutW(dc, plot.left + 4, y, label, lstrlenW(label));
                    SelectObject(dc, previous); DeleteObject(pen);
                };
                drawLevel(history.back().nearestSupport, L"Support", palette.down);
                drawLevel(history.back().nearestResistance, L"Resistance", palette.up);
            }
            for (int i = 0; i < count; ++i) {
                const auto& r = history[indices.at(state->kLines[i].date)];
                bool buy = r.technicalSignal == "EARLY_BUY" || r.technicalSignal == "BUY_CONFIRMATION" ||
                           r.technicalSignal == "STRONG_BUY_CONFIRMATION";
                bool sell = r.technicalSignal == "PARTIAL_SELL" || r.technicalSignal == "SELL_CONFIRMATION" ||
                            r.technicalSignal == "STRONG_SELL_CONFIRMATION";
                if (!buy && !sell) continue;
                SetTextColor(dc, buy ? palette.up : palette.down);
                const wchar_t* label = buy ? L"BUY*" : L"SELL*";
                TextOutW(dc, plot.left + i * spacing, yOf(state->kLines[i].close), label, lstrlenW(label));
            }
            RestoreDC(dc, savedDc);
        }
        if (state->hoveredTradeMarker &&
            *state->hoveredTradeMarker >= state->chartTradeMarkers.size()) {
            state->hoveredTradeMarker.reset();
        }
        if (state->hoveredCandleMarker &&
            *state->hoveredCandleMarker >= state->chartCandleMarkers.size()) {
            state->hoveredCandleMarker.reset();
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette.text);
        std::wstring mode = state->currentKlt == 1 ? L"1 分钟走势" : L"日 K 线";
        double latestPrice = state->kLines.back().close;
        double changeReference = referencePrice;
        if (state->currentKlt != 1 && state->kLines.size() > 1) {
            changeReference = state->kLines[state->kLines.size() - 2].close;
        }
        double currentChangePercent = changeReference > 0.0
                                          ? (latestPrice / changeReference - 1.0) * 100.0
                                          : 0.0;
        std::wstring title = mode + L"  " + utf8ToWide(state->currentSymbol);
        if (state->dataMode == MarketDataMode::Replay && state->replay.ready()) {
            title += L"  回放 " + utf8ToWide(state->replay.date()) + L" " +
                     utf8ToWide(state->replay.currentTime());
        }
        int identityWidth = width < 1100 ? 190 : 260;
        int metricWidth = width < 1100 ? 118 : 142;
        RECT titleRect{rect.left + 12, rect.top + 3,
                       rect.left + 12 + identityWidth, rect.top + 32};
        HFONT headingPreviousFont = state->chartAxisFont
                                        ? static_cast<HFONT>(SelectObject(dc, state->chartAxisFont))
                                        : nullptr;
        SetTextColor(dc, palette.text);
        DrawTextW(dc, title.c_str(), -1, &titleRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT highRect{titleRect.right + 8, rect.top + 3,
                      titleRect.right + 8 + metricWidth, rect.top + 32};
        SetTextColor(dc, palette.up);
        std::wstring highText = L"最高 " + formatNumber(maxHigh, pricePrecision);
        if (width >= 950) DrawTextW(dc, highText.c_str(), -1, &highRect,
                                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        RECT lowRect{highRect.right + 8, rect.top + 3,
                     highRect.right + 8 + metricWidth, rect.top + 32};
        SetTextColor(dc, palette.down);
        std::wstring lowText = L"最低 " + formatNumber(minLow, pricePrecision);
        if (width >= 950) DrawTextW(dc, lowText.c_str(), -1, &lowRect,
                                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (headingPreviousFont) {
            SelectObject(dc, headingPreviousFont);
        }
        RECT changeRect{std::max(titleRect.right + 8, rect.right - 304), rect.top + 2,
                        rect.right - 14, rect.top + 31};
        SetTextColor(dc, currentChangePercent > 0.0001 ? palette.up :
                         currentChangePercent < -0.0001 ? palette.down : palette.muted);
        std::wstring changeText = L"最新 " + formatNumber(latestPrice, pricePrecision) +
                                  L"    涨跌幅 " +
                                  std::wstring(currentChangePercent > 0.0001 ? L"+" : L"") +
                                  formatNumber(currentChangePercent, 2) + L"%";
        if (width < 550) changeText = L"最新 " + formatNumber(latestPrice, pricePrecision);
        HFONT valuePreviousFont = state->chartValueFont
                                      ? static_cast<HFONT>(SelectObject(dc, state->chartValueFont))
                                      : nullptr;
        DrawTextW(dc, changeText.c_str(), -1, &changeRect,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (valuePreviousFont) {
            SelectObject(dc, valuePreviousFont);
        }

        SetTextColor(dc, palette.muted);
        RECT axisRect{plot.left, plot.bottom + 5, plot.right, rect.bottom};
        if (state->currentKlt == 1) {
            DrawTextW(dc, L"09:30", -1, &axisRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
            RECT morningMidRect{plot.left + plotWidth / 4 - 50, axisRect.top,
                                plot.left + plotWidth / 4 + 50, axisRect.bottom};
            if (plotWidth >= 400) DrawTextW(dc, L"10:30", -1, &morningMidRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
            RECT sessionRect{plot.left + plotWidth / 2 - 80, axisRect.top,
                             plot.left + plotWidth / 2 + 80, axisRect.bottom};
            DrawTextW(dc, L"11:30 / 13:00", -1, &sessionRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
            RECT afternoonMidRect{plot.left + plotWidth * 3 / 4 - 50, axisRect.top,
                                  plot.left + plotWidth * 3 / 4 + 50, axisRect.bottom};
            if (plotWidth >= 400) DrawTextW(dc, L"14:00", -1, &afternoonMidRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
            DrawTextW(dc, L"15:00", -1, &axisRect, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        } else {
            // 三个日期使用互不重叠的矩形，防止窗口较窄时相互覆盖。
            constexpr LONG labelWidth = 120;
            LONG center = plot.left + plotWidth / 2;
            RECT firstRect{plot.left, axisRect.top,
                           std::min(plot.right, plot.left + labelWidth), axisRect.bottom};
            RECT middleRect{std::max(plot.left, center - labelWidth / 2), axisRect.top,
                            std::min(plot.right, center + labelWidth / 2), axisRect.bottom};
            RECT lastRect{std::max(plot.left, plot.right - labelWidth), axisRect.top,
                          plot.right, axisRect.bottom};
            std::wstring firstDate = formatAxisDate(state->kLines.front().date);
            std::wstring middleDate = formatAxisDate(state->kLines[state->kLines.size() / 2].date);
            std::wstring lastDate = formatAxisDate(state->kLines.back().date);
            constexpr UINT dateFlags = DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
            DrawTextW(dc, firstDate.c_str(), -1, &firstRect, dateFlags | DT_LEFT);
            DrawTextW(dc, middleDate.c_str(), -1, &middleRect, dateFlags | DT_CENTER);
            DrawTextW(dc, lastDate.c_str(), -1, &lastRect, dateFlags | DT_RIGHT);
        }

        // 悬停提示在坐标轴之后绘制，保证提示框不会被网格线或标签覆盖。
        if (state->hoveredTradeMarker &&
            *state->hoveredTradeMarker < state->chartTradeMarkers.size()) {
            const ChartTradeMarker& marker = state->chartTradeMarkers[*state->hoveredTradeMarker];
            constexpr int tooltipWidth = 250;
            constexpr int tooltipHeight = 72;
            int left = marker.anchor.x + 12;
            if (left + tooltipWidth > rect.right - 8) {
                left = marker.anchor.x - tooltipWidth - 12;
            }
            left = std::clamp(left, 8,
                              std::max(8, static_cast<int>(rect.right) - tooltipWidth - 8));
            int top = marker.anchor.y - tooltipHeight - 12;
            if (top < 8) {
                top = marker.anchor.y + 12;
            }
            top = std::clamp(top, 8,
                             std::max(8, static_cast<int>(rect.bottom) - tooltipHeight - 8));
            RECT tooltip{left, top, left + tooltipWidth, top + tooltipHeight};
            HBRUSH tooltipBrush = CreateSolidBrush(palette.sidebar);
            HPEN tooltipPen = CreatePen(PS_SOLID, 1, palette.accent);
            HGDIOBJ previousBrush = SelectObject(dc, tooltipBrush);
            HGDIOBJ previousPen = SelectObject(dc, tooltipPen);
            RoundRect(dc, tooltip.left, tooltip.top, tooltip.right, tooltip.bottom, 6, 6);
            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            DeleteObject(tooltipBrush);
            DeleteObject(tooltipPen);

            bool buy = marker.trade.side == SignalType::Buy;
            SetTextColor(dc, buy ? palette.up : palette.down);
            RECT directionRect{left + 10, top + 6, left + tooltipWidth - 10, top + 26};
            std::wstring direction;
            if (marker.trade.source == TradeSource::Manual) {
                direction = buy ? L"买入成交" : L"卖出成交";
            } else {
                direction = buy ? L"模拟买入" : L"模拟卖出";
            }
            direction += L"  " + std::to_wstring(marker.trade.quantity) + L" 股";
            DrawTextW(dc, direction.c_str(), -1, &directionRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(dc, palette.text);
            int precision = isFundLikeSymbol(marker.trade.symbol) ? 3 : 2;
            RECT priceRect{left + 10, top + 26, left + tooltipWidth - 10, top + 46};
            std::wstring price = L"成交价 " + formatNumber(marker.trade.price, precision) +
                                 L"  费用 " + formatNumber(tradeFees(marker.trade), 2);
            DrawTextW(dc, price.c_str(), -1, &priceRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(dc, palette.muted);
            RECT timeRect{left + 10, top + 46, left + tooltipWidth - 10, top + 66};
            std::wstring time = utf8ToWide(marker.trade.time);
            DrawTextW(dc, time.c_str(), -1, &timeRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        } else if (state->hoveredCandleMarker &&
                   *state->hoveredCandleMarker < state->chartCandleMarkers.size()) {
            const ChartCandleMarker& marker = state->chartCandleMarkers[*state->hoveredCandleMarker];
            const KLine& line = state->kLines[marker.lineIndex];
            double previousClose = marker.lineIndex > 0
                                       ? state->kLines[marker.lineIndex - 1].close
                                       : line.open;
            double change = line.close - previousClose;
            double percent = previousClose > 0.0 ? change / previousClose * 100.0 : 0.0;
            constexpr int tooltipWidth = 292;
            constexpr int tooltipHeight = 112;
            int left = marker.anchor.x + 12;
            if (left + tooltipWidth > rect.right - 8) {
                left = marker.anchor.x - tooltipWidth - 12;
            }
            left = std::clamp(left, 8,
                              std::max(8, static_cast<int>(rect.right) - tooltipWidth - 8));
            int top = marker.anchor.y - tooltipHeight / 2;
            top = std::clamp(top, 8,
                             std::max(8, static_cast<int>(rect.bottom) - tooltipHeight - 8));
            RECT tooltip{left, top, left + tooltipWidth, top + tooltipHeight};
            HBRUSH tooltipBrush = CreateSolidBrush(palette.sidebar);
            HPEN tooltipPen = CreatePen(PS_SOLID, 1, palette.accent);
            HGDIOBJ previousBrush = SelectObject(dc, tooltipBrush);
            HGDIOBJ previousTooltipPen = SelectObject(dc, tooltipPen);
            RoundRect(dc, tooltip.left, tooltip.top, tooltip.right, tooltip.bottom, 6, 6);
            SelectObject(dc, previousBrush);
            SelectObject(dc, previousTooltipPen);
            DeleteObject(tooltipBrush);
            DeleteObject(tooltipPen);

            SetTextColor(dc, change > 0.0 ? palette.up : change < 0.0 ? palette.down : palette.text);
            RECT dateRect{left + 10, top + 6, left + tooltipWidth - 10, top + 28};
            std::wstring heading = formatAxisDate(line.date) + L"  " +
                                   (change > 0.0 ? L"+" : L"") +
                                   formatNumber(change, pricePrecision) + L"  (" +
                                   (percent > 0.0 ? L"+" : L"") +
                                   formatNumber(percent, 2) + L"%)";
            DrawTextW(dc, heading.c_str(), -1, &dateRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(dc, palette.text);
            RECT openCloseRect{left + 10, top + 30, left + tooltipWidth - 10, top + 52};
            std::wstring openClose = L"开盘 " + formatNumber(line.open, pricePrecision) +
                                     L"    收盘 " + formatNumber(line.close, pricePrecision);
            DrawTextW(dc, openClose.c_str(), -1, &openCloseRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            RECT highLowRect{left + 10, top + 54, left + tooltipWidth - 10, top + 76};
            std::wstring highLow = L"最高 " + formatNumber(line.high, pricePrecision) +
                                   L"    最低 " + formatNumber(line.low, pricePrecision);
            DrawTextW(dc, highLow.c_str(), -1, &highLowRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(dc, palette.muted);
            RECT volumeRect{left + 10, top + 78, left + tooltipWidth - 10, top + 102};
            std::wstring volume = L"成交量 " + formatLarge(line.volume);
            DrawTextW(dc, volume.c_str(), -1, &volumeRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        present();
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        AppState* state = app(GetParent(hwnd));
        if (state) {
            POINT mouse{static_cast<short>(LOWORD(lParam)),
                        static_cast<short>(HIWORD(lParam))};
            std::optional<size_t> hovered;
            // 后绘制的标记位于上层，重叠时优先选择最后一个。
            for (size_t index = state->chartTradeMarkers.size(); index > 0; --index) {
                if (PtInRect(&state->chartTradeMarkers[index - 1].bounds, mouse)) {
                    hovered = index - 1;
                    break;
                }
            }
            std::optional<size_t> hoveredCandle;
            if (!hovered && state->currentKlt != 1) {
                for (size_t index = state->chartCandleMarkers.size(); index > 0; --index) {
                    if (PtInRect(&state->chartCandleMarkers[index - 1].bounds, mouse)) {
                        hoveredCandle = index - 1;
                        break;
                    }
                }
            }
            if (hovered != state->hoveredTradeMarker ||
                hoveredCandle != state->hoveredCandleMarker) {
                state->hoveredTradeMarker = hovered;
                state->hoveredCandleMarker = hoveredCandle;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (!state->trackingChartMouse) {
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tracking);
                state->trackingChartMouse = true;
            }
        }
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        AppState* state = app(GetParent(hwnd));
        if (state) {
            state->trackingChartMouse = false;
            if (state->hoveredTradeMarker || state->hoveredCandleMarker) {
                state->hoveredTradeMarker.reset();
                state->hoveredCandleMarker.reset();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        AppState* state = app(GetParent(hwnd));
        if (state && (state->hoveredTradeMarker || state->hoveredCandleMarker)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK HeaderProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                            UINT_PTR subclassId, DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<AppState*>(referenceData);
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT && state) {
        // 原生 Header 的深色支持不稳定，因此逐列自绘文本和底部分隔线。
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        ThemePalette palette = themePalette();
        HBRUSH background = CreateSolidBrush(palette.surface);
        FillRect(dc, &client, background);
        DeleteObject(background);

        HFONT oldFont = static_cast<HFONT>(SelectObject(dc, state->font));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, palette.muted);
        int itemCount = Header_GetItemCount(hwnd);
        for (int index = 0; index < itemCount; ++index) {
            RECT itemRect{};
            if (!Header_GetItemRect(hwnd, index, &itemRect)) {
                continue;
            }
            wchar_t text[64]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = text;
            item.cchTextMax = 64;
            Header_GetItem(hwnd, index, &item);
            RECT textRect = itemRect;
            textRect.left += 8;
            DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SelectObject(dc, oldFont);

        HPEN pen = CreatePen(PS_SOLID, 1, palette.border);
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        MoveToEx(dc, client.left, client.bottom - 1, nullptr);
        LineTo(dc, client.right, client.bottom - 1);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, HeaderProc, subclassId);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace ashare
