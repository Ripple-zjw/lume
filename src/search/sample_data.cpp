#include "search/result.h"

std::vector<ResultRow> SampleResults() {
  return {
      {L"微信", L"C:\\Program Files\\Tencent\\WeChat\\WeChat.exe", L"微", 0x14C463},
      {L"Visual Studio Code", L"C:\\Users\\13152\\AppData\\Local\\Programs\\VSCode\\Code.exe", L"V",
       0x2F80ED},
      {L"下载", L"C:\\Users\\13152\\Downloads", L"下", 0x8B5CF6},
      {L"记事本", L"C:\\Windows\\System32\\notepad.exe", L"记", 0xF59E0B},
      {L"计算器", L"C:\\Windows\\System32\\calc.exe", L"计", 0x10B981},
      {L"Windows 终端", L"C:\\Windows\\System32\\wt.exe", L"终", 0x6366F1},
      {L"任务管理器", L"C:\\Windows\\System32\\Taskmgr.exe", L"任", 0xEF4444},
      {L"设置", L"ms-settings:", L"设", 0x0EA5E9},
      {L"截图工具", L"C:\\Windows\\System32\\SnippingTool.exe", L"截", 0xEC4899},
      {L"画图", L"C:\\Windows\\System32\\mspaint.exe", L"画", 0x84CC16},
      {L"控制面板", L"control.exe", L"控", 0x64748B},
      {L"回收站", L"shell:RecycleBinFolder", L"回", 0x7C3AED},
  };
}
