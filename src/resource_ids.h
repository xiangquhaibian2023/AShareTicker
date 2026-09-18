#pragma once

#include <windows.h>

namespace ashare {

// 主窗口控件 ID。
constexpr int IDC_CODE = 1001;
constexpr int IDC_SEARCH = 1002;
constexpr int IDC_ADD = 1003;
constexpr int IDC_DELETE = 1004;
constexpr int IDC_REFRESH = 1005;
constexpr int IDC_LIST = 1007;
constexpr int IDC_STATUS = 1008;
constexpr int IDC_KLINE = 1009;
constexpr int IDC_DAY_KLINE = 1010;
constexpr int IDC_MIN_KLINE = 1011;
constexpr int IDC_INDEX_LIST = 1012;
constexpr int IDC_INDEX_LABEL = 1013;
constexpr int IDC_FAVORITES_LABEL = 1014;
constexpr int IDC_TITLE = 1016;
constexpr int IDC_VIEW_FAVORITES = 1017;
constexpr int IDC_VIEW_HOLDINGS = 1018;
constexpr int IDC_VIEW_STRATEGY = 1019;
constexpr int IDC_HOLDING_QUANTITY = 1020;
constexpr int IDC_HOLDING_COST = 1021;
constexpr int IDC_SAVE_HOLDING = 1022;
constexpr int IDC_TOGGLE_STRATEGY = 1023;
constexpr int IDC_PAGE_TITLE = 1024;
constexpr int IDC_OPEN_LOGS = 1025;
constexpr int IDC_CODE_LABEL = 1026;
constexpr int IDC_QUANTITY_LABEL = 1027;
constexpr int IDC_COST_LABEL = 1028;
constexpr int IDC_TODAY_TRADES = 1029;
constexpr int IDC_STRATEGY_COMBO = 1030;
constexpr int IDC_MANAGE_STRATEGIES = 1031;
constexpr int IDC_STRATEGY_LABEL = 1032;
constexpr int IDC_DATA_MODE_LABEL = 1033;
constexpr int IDC_DATA_MODE_COMBO = 1034;
constexpr int IDC_REPLAY_DATE_LABEL = 1035;
constexpr int IDC_REPLAY_DATE = 1036;
constexpr int IDC_REPLAY_LOAD = 1037;
constexpr int IDC_REPLAY_PLAY = 1038;
constexpr int IDC_REPLAY_STEP = 1039;
constexpr int IDC_REPLAY_RESET = 1040;
constexpr int IDC_SIGNAL_HISTORY_LABEL = 1041;
constexpr int IDC_SIGNAL_HISTORY_LIST = 1042;
constexpr int IDC_ACCOUNT_FUNDS = 1043;
constexpr int IDC_DETAIL_TOGGLE = 1044;
constexpr int IDC_HOLDING_SUMMARY = 1045;
constexpr int IDC_VIEW_RESEARCH = 1046;
constexpr int IDC_VIEW_SCREENER = 1047;
constexpr int IDC_VIEW_TRADE_ANALYSIS = 1048;
constexpr int IDC_TECHNICAL_SWING_LABEL = 1049;
constexpr int IDC_TECHNICAL_SWING_DETAILS = 1050;
constexpr int IDC_TECHNICAL_SWING_TOGGLE = 1051;

// 券商成交与持仓优化页面控件 ID。
constexpr int IDC_ANALYSIS_IMPORT = 6001;
constexpr int IDC_ANALYSIS_SYNC = 6002;
constexpr int IDC_ANALYSIS_REFRESH = 6003;
constexpr int IDC_ANALYSIS_ISSUES = 6004;
constexpr int IDC_ANALYSIS_SUMMARY = 6005;
constexpr int IDC_ANALYSIS_DETAILS = 6006;
constexpr int IDC_ANALYSIS_STATUS = 6007;
constexpr int IDC_ANALYSIS_ADVICE_LIST = 6008;
constexpr int IDC_ANALYSIS_TRADE_LIST = 6009;

// T0 策略选股页面控件 ID。
constexpr int IDC_SCREENER_UNIVERSE = 5201;
constexpr int IDC_SCREENER_LIMIT = 5202;
constexpr int IDC_SCREENER_RUN = 5203;
constexpr int IDC_SCREENER_ADD_FAVORITE = 5204;
constexpr int IDC_SCREENER_ADD_STRATEGY = 5205;
constexpr int IDC_SCREENER_ISSUES = 5206;
constexpr int IDC_SCREENER_LIST = 5207;
constexpr int IDC_SCREENER_STATUS = 5208;

// 算法研究页面控件 ID。
constexpr int IDC_RESEARCH_STRATEGY = 5001;
constexpr int IDC_RESEARCH_SCOPE = 5002;
constexpr int IDC_RESEARCH_START_DATE = 5003;
constexpr int IDC_RESEARCH_END_DATE = 5004;
constexpr int IDC_RESEARCH_RUN = 5005;
constexpr int IDC_RESEARCH_DAY_FILTER = 5006;
constexpr int IDC_RESEARCH_LIST = 5007;
constexpr int IDC_RESEARCH_STATUS = 5008;
constexpr int IDC_RESEARCH_ISSUES = 5009;
constexpr int IDC_RESEARCH_SYMBOL_FILTER = 5010;
constexpr int IDC_RESEARCH_SETTINGS = 5011;

// 算法研究期初账户参数弹窗控件 ID。
constexpr int IDC_RESEARCH_SETTING_SYMBOL = 5101;
constexpr int IDC_RESEARCH_SETTING_QUANTITY = 5102;
constexpr int IDC_RESEARCH_SETTING_COST = 5103;
constexpr int IDC_RESEARCH_SETTING_CASH = 5104;
constexpr int IDC_RESEARCH_SETTING_TEMPLATE = 5105;
constexpr int IDC_RESEARCH_SETTING_COMMISSION = 5106;
constexpr int IDC_RESEARCH_SETTING_MINIMUM = 5107;
constexpr int IDC_RESEARCH_SETTING_STAMP = 5108;
constexpr int IDC_RESEARCH_SETTING_TRANSFER = 5109;
constexpr int IDC_RESEARCH_SETTING_SLIPPAGE = 5110;

// 分笔成交管理对话框控件 ID。
constexpr int IDC_TRADE_LIST = 4001;
constexpr int IDC_TRADE_SYMBOL = 4002;
constexpr int IDC_TRADE_SIDE = 4003;
constexpr int IDC_TRADE_TIME = 4004;
constexpr int IDC_TRADE_QUANTITY = 4005;
constexpr int IDC_TRADE_PRICE = 4006;
constexpr int IDC_TRADE_COMMISSION = 4007;
constexpr int IDC_TRADE_STAMP_DUTY = 4008;
constexpr int IDC_TRADE_TRANSFER_FEE = 4009;
constexpr int IDC_TRADE_OTHER_FEES = 4010;
constexpr int IDC_TRADE_ADD = 4011;
constexpr int IDC_TRADE_UPDATE = 4012;
constexpr int IDC_TRADE_DELETE = 4013;

// 策略管理对话框控件 ID。
constexpr int IDC_STRATEGY_LIST = 4101;
constexpr int IDC_STRATEGY_NAME = 4102;
constexpr int IDC_STRATEGY_KIND = 4103;
constexpr int IDC_STRATEGY_ADD = 4104;
constexpr int IDC_STRATEGY_DELETE = 4105;

constexpr int IDC_FUNDS_TOTAL_ASSETS = 4201;
constexpr int IDC_FUNDS_AVAILABLE_CASH = 4202;

// 持仓基准编辑弹窗控件 ID。
constexpr int IDC_HOLDING_DIALOG_SYMBOL = 4301;
constexpr int IDC_HOLDING_DIALOG_QUANTITY = 4302;
constexpr int IDC_HOLDING_DIALOG_COST = 4303;

// 定时刷新与自定义窗口消息。
constexpr int REFRESH_TIMER = 2001;
constexpr int REPLAY_TIMER = 2002;
constexpr UINT WM_APP_STRATEGY_RESULT = WM_APP + 1;
constexpr UINT WM_APP_RESEARCH_PROGRESS = WM_APP + 2;
constexpr UINT WM_APP_RESEARCH_RESULT = WM_APP + 3;
constexpr UINT WM_APP_SCREENER_RESULT = WM_APP + 4;
constexpr UINT WM_APP_TRADE_ANALYSIS_RESULT = WM_APP + 5;

}  // namespace ashare
