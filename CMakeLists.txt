cmake_minimum_required(VERSION 3.16)
project(AShareTicker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(ashare_client WIN32
    src/main.cpp
    src/main_window.cpp
    src/dashboard_view.cpp
    src/market_controller.cpp
    src/chart_view.cpp
    src/dialogs.cpp
    src/trade_dialogs.cpp
    src/trade_ledger.cpp
    src/ui_theme.cpp
    src/utils.cpp
    src/http_client.cpp
    src/quote_provider.cpp
    src/replay_engine.cpp
    src/research_analysis.cpp
    src/research_settings_dialog.cpp
    src/research_view.cpp
    src/strategy_backtest.cpp
    src/signal_history.cpp
    src/strategy_simulator.cpp
    src/storage.cpp
    src/stock_screener.cpp
    src/stock_screener_view.cpp
    src/broker_statement.cpp
    src/position_advisor.cpp
    src/trade_analysis_view.cpp
    src/t0_algorithm.cpp
    src/trading_algorithm.cpp
    src/technical_swing_strategy.cpp
)

target_include_directories(ashare_client PRIVATE src)

include(CTest)
if(BUILD_TESTING)
    add_executable(ashare_technical_swing_tests
        tests/technical_swing_strategy_tests.cpp
        src/technical_swing_strategy.cpp
        src/trading_algorithm.cpp
        src/t0_algorithm.cpp
        src/storage.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_technical_swing_tests PRIVATE src)
    target_compile_definitions(ashare_technical_swing_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(MINGW)
        target_link_options(ashare_technical_swing_tests PRIVATE -static)
    endif()
    add_test(NAME ashare_technical_swing_tests COMMAND ashare_technical_swing_tests)
    add_executable(ashare_strategy_tests
        tests/trading_algorithm_tests.cpp
        src/t0_algorithm.cpp
        src/trading_algorithm.cpp
        src/technical_swing_strategy.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_strategy_tests PRIVATE src)
    target_compile_definitions(ashare_strategy_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_strategy_tests COMMAND ashare_strategy_tests)

    add_executable(ashare_replay_tests
        tests/replay_engine_tests.cpp
        src/replay_engine.cpp
        src/quote_provider.cpp
        src/http_client.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_replay_tests PRIVATE src)
    target_compile_definitions(ashare_replay_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_replay_tests COMMAND ashare_replay_tests)

    add_executable(ashare_strategy_simulator_tests
        tests/strategy_simulator_tests.cpp
        src/strategy_simulator.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_strategy_simulator_tests PRIVATE src)
    target_compile_definitions(ashare_strategy_simulator_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_strategy_simulator_tests COMMAND ashare_strategy_simulator_tests)

    add_executable(ashare_storage_tests
        tests/storage_tests.cpp
        src/storage.cpp
        src/t0_algorithm.cpp
        src/trading_algorithm.cpp
        src/technical_swing_strategy.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_storage_tests PRIVATE src)
    target_compile_definitions(ashare_storage_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_storage_tests COMMAND ashare_storage_tests)

    add_executable(ashare_trade_ledger_tests
        tests/trade_ledger_tests.cpp
        src/trade_ledger.cpp
    )
    target_include_directories(ashare_trade_ledger_tests PRIVATE src)
    add_test(NAME ashare_trade_ledger_tests COMMAND ashare_trade_ledger_tests)

    add_executable(ashare_signal_history_tests
        tests/signal_history_tests.cpp
        src/signal_history.cpp
    )
    target_include_directories(ashare_signal_history_tests PRIVATE src)
    add_test(NAME ashare_signal_history_tests COMMAND ashare_signal_history_tests)

    add_executable(ashare_strategy_backtest_tests
        tests/strategy_backtest_tests.cpp
        src/http_client.cpp
        src/quote_provider.cpp
        src/replay_engine.cpp
        src/strategy_backtest.cpp
        src/strategy_simulator.cpp
        src/t0_algorithm.cpp
        src/trading_algorithm.cpp
        src/technical_swing_strategy.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_strategy_backtest_tests PRIVATE src)
    target_compile_definitions(ashare_strategy_backtest_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_strategy_backtest_tests COMMAND ashare_strategy_backtest_tests)

    add_executable(ashare_research_analysis_tests
        tests/research_analysis_tests.cpp
        src/http_client.cpp
        src/quote_provider.cpp
        src/replay_engine.cpp
        src/research_analysis.cpp
        src/strategy_backtest.cpp
        src/strategy_simulator.cpp
        src/t0_algorithm.cpp
        src/trading_algorithm.cpp
        src/technical_swing_strategy.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_research_analysis_tests PRIVATE src)
    target_compile_definitions(ashare_research_analysis_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_research_analysis_tests COMMAND ashare_research_analysis_tests)

    add_executable(ashare_stock_screener_tests
        tests/stock_screener_tests.cpp
        src/stock_screener.cpp
    )
    target_include_directories(ashare_stock_screener_tests PRIVATE src)
    add_test(NAME ashare_stock_screener_tests COMMAND ashare_stock_screener_tests)

    add_executable(ashare_broker_statement_tests
        tests/broker_statement_tests.cpp
        src/broker_statement.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_broker_statement_tests PRIVATE src)
    target_compile_definitions(ashare_broker_statement_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_broker_statement_tests COMMAND ashare_broker_statement_tests)

    add_executable(ashare_position_advisor_tests
        tests/position_advisor_tests.cpp
        src/position_advisor.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_position_advisor_tests PRIVATE src)
    target_compile_definitions(ashare_position_advisor_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    add_test(NAME ashare_position_advisor_tests COMMAND ashare_position_advisor_tests)

    add_executable(ashare_t0_backtest
        tools/t0_backtest.cpp
        src/http_client.cpp
        src/quote_provider.cpp
        src/replay_engine.cpp
        src/storage.cpp
        src/strategy_backtest.cpp
        src/strategy_simulator.cpp
        src/t0_algorithm.cpp
        src/trading_algorithm.cpp
        src/technical_swing_strategy.cpp
        src/utils.cpp
    )
    target_include_directories(ashare_t0_backtest PRIVATE src)
    target_compile_definitions(ashare_t0_backtest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

if(WIN32)
    target_link_libraries(ashare_client PRIVATE winhttp comctl32 shell32 gdi32 comdlg32)
    if(BUILD_TESTING)
        target_link_libraries(ashare_replay_tests PRIVATE winhttp)
        target_link_libraries(ashare_t0_backtest PRIVATE winhttp)
        target_link_libraries(ashare_strategy_backtest_tests PRIVATE winhttp)
        target_link_libraries(ashare_research_analysis_tests PRIVATE winhttp)
    endif()
    target_compile_definitions(ashare_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if(MINGW)
        target_link_options(ashare_client PRIVATE -municode -static)
        if(BUILD_TESTING)
            target_link_options(ashare_strategy_tests PRIVATE -static)
            target_link_options(ashare_replay_tests PRIVATE -static)
            target_link_options(ashare_strategy_simulator_tests PRIVATE -static)
            target_link_options(ashare_storage_tests PRIVATE -static)
            target_link_options(ashare_trade_ledger_tests PRIVATE -static)
            target_link_options(ashare_signal_history_tests PRIVATE -static)
            target_link_options(ashare_t0_backtest PRIVATE -static)
            target_link_options(ashare_strategy_backtest_tests PRIVATE -static)
            target_link_options(ashare_research_analysis_tests PRIVATE -static)
            target_link_options(ashare_stock_screener_tests PRIVATE -static)
            target_link_options(ashare_broker_statement_tests PRIVATE -static)
            target_link_options(ashare_position_advisor_tests PRIVATE -static)
        endif()
    endif()
else()
    message(FATAL_ERROR "This prototype uses WinHTTP and currently targets Windows.")
endif()
