@echo off
setlocal
pushd "%~dp0"

if not exist build mkdir build
cl /nologo /EHsc /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^
    src\main.cpp ^
    src\main_window.cpp ^
    src\dashboard_view.cpp ^
    src\market_controller.cpp ^
    src\chart_view.cpp ^
    src\dialogs.cpp ^
    src\trade_dialogs.cpp ^
    src\trade_ledger.cpp ^
    src\ui_theme.cpp ^
    src\utils.cpp ^
    src\http_client.cpp ^
    src\quote_provider.cpp ^
    src\replay_engine.cpp ^
    src\research_analysis.cpp ^
    src\research_settings_dialog.cpp ^
    src\research_view.cpp ^
    src\strategy_backtest.cpp ^
    src\signal_history.cpp ^
    src\strategy_simulator.cpp ^
    src\storage.cpp ^
    src\stock_screener.cpp ^
    src\stock_screener_view.cpp ^
    src\broker_statement.cpp ^
    src\position_advisor.cpp ^
    src\trade_analysis_view.cpp ^
    src\t0_algorithm.cpp ^
    src\technical_swing_strategy.cpp ^
    src\trading_algorithm.cpp ^
    /Fo"build\\" /Fe"build\ashare_client.exe" ^
    /link winhttp.lib comctl32.lib shell32.lib gdi32.lib comdlg32.lib /subsystem:windows
if errorlevel 1 goto :build_failed

echo Built build\ashare_client.exe
popd
exit /b 0

:build_failed
echo Build failed.
popd
exit /b 1
