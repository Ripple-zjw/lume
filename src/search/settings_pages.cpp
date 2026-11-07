#include "search/settings_pages.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"

namespace {

// 分类配色：只在图标取不到（非标准系统）时显示为首字色块，用来区分类型。
constexpr uint32_t kColorSystem = 0x6EA8FF;
constexpr uint32_t kColorDevices = 0x14C463;
constexpr uint32_t kColorNetwork = 0x0EA5E9;
constexpr uint32_t kColorPersonalization = 0x8B5CF6;
constexpr uint32_t kColorApps = 0x34D399;
constexpr uint32_t kColorAccounts = 0x6366F1;
constexpr uint32_t kColorTimeLanguage = 0x10B981;
constexpr uint32_t kColorGaming = 0xEF4444;
constexpr uint32_t kColorPrivacy = 0xF59E0B;
constexpr uint32_t kColorUpdateSecurity = 0x3B82F6;
constexpr uint32_t kColorAccessibility = 0x2F80ED;

constexpr size_t kMaxKeywords = 8;

struct SettingsPageDef {
  const wchar_t* title;
  const wchar_t* uri;
  const wchar_t* category;
  uint32_t color;
  const wchar_t* keywords[kMaxKeywords];
};

// 顺序即同一查询同分时的兜底顺序（标题长度/标题仍会参与比较），按分类聚在一起便于维护。
// uri 都来自 Microsoft Learn 的 ms-settings 表；标为「硬件专属/已废弃」的一律不收。
constexpr SettingsPageDef kSettingsPages[] = {
    // 系统
    {L"显示", L"ms-settings:display", L"系统", kColorSystem,
     {L"屏幕", L"分辨率", L"缩放", L"显示器", L"display", L"screen", L"resolution", L"scaling"}},
    {L"声音", L"ms-settings:sound", L"系统", kColorSystem,
     {L"音频", L"扬声器", L"音量", L"sound", L"audio", L"volume", L"speaker"}},
    {L"音量混合器", L"ms-settings:apps-volume", L"系统", kColorSystem,
     {L"应用音量", L"合成器", L"mixer", L"volume"}},
    {L"通知", L"ms-settings:notifications", L"系统", kColorSystem,
     {L"提醒", L"通知中心", L"notifications", L"notification"}},
    {L"专注助手", L"ms-settings:quiethours", L"系统", kColorSystem,
     {L"免打扰", L"勿扰", L"专注", L"focus", L"assist", L"quiet"}},
    {L"电源和电池", L"ms-settings:powersleep", L"系统", kColorSystem,
     {L"电源", L"睡眠", L"休眠", L"电池", L"省电", L"power", L"sleep", L"battery"}},
    {L"存储", L"ms-settings:storagesense", L"系统", kColorSystem,
     {L"存储空间", L"磁盘空间", L"清理", L"storage", L"disk", L"space"}},
    {L"磁盘和卷", L"ms-settings:disksandvolumes", L"系统", kColorSystem,
     {L"磁盘", L"分区", L"卷", L"磁盘管理", L"disks", L"volumes", L"partition"}},
    {L"多任务", L"ms-settings:multitasking", L"系统", kColorSystem,
     {L"分屏", L"贴靠", L"窗口", L"multitasking", L"snap"}},
    {L"剪贴板", L"ms-settings:clipboard", L"系统", kColorSystem,
     {L"剪贴板历史", L"复制粘贴", L"clipboard"}},
    {L"远程桌面", L"ms-settings:remotedesktop", L"系统", kColorSystem,
     {L"远程", L"桌面", L"rdp", L"remote", L"desktop"}},
    {L"关于", L"ms-settings:about", L"系统", kColorSystem,
     {L"系统信息", L"版本", L"设备规格", L"计算机名", L"about", L"system", L"info", L"version"}},
    {L"图形设置", L"ms-settings:display-advancedgraphics", L"系统", kColorSystem,
     {L"显卡", L"gpu", L"高性能", L"graphics"}},

    // 蓝牙和其他设备
    {L"蓝牙", L"ms-settings:bluetooth", L"蓝牙和其他设备", kColorDevices,
     {L"蓝牙设备", L"配对", L"bluetooth", L"pair"}},
    {L"打印机和扫描仪", L"ms-settings:printers", L"蓝牙和其他设备", kColorDevices,
     {L"打印机", L"扫描仪", L"printer", L"scanner"}},
    {L"已连接的设备", L"ms-settings:connecteddevices", L"蓝牙和其他设备", kColorDevices,
     {L"设备", L"外设", L"devices", L"connected"}},
    {L"鼠标", L"ms-settings:mousetouchpad", L"蓝牙和其他设备", kColorDevices,
     {L"指针", L"光标", L"mouse", L"cursor"}},
    {L"输入", L"ms-settings:typing", L"蓝牙和其他设备", kColorDevices,
     {L"键盘", L"拼写", L"typing", L"keyboard"}},
    {L"相机", L"ms-settings:camera", L"蓝牙和其他设备", kColorDevices,
     {L"摄像头", L"camera", L"webcam"}},

    // 网络和 Internet
    {L"网络状态", L"ms-settings:network-status", L"网络和 Internet", kColorNetwork,
     {L"网络", L"连接", L"状态", L"network", L"internet", L"status"}},
    {L"Wi-Fi", L"ms-settings:network-wifi", L"网络和 Internet", kColorNetwork,
     {L"无线", L"无线网", L"wifi", L"wireless"}},
    {L"以太网", L"ms-settings:network-ethernet", L"网络和 Internet", kColorNetwork,
     {L"有线", L"网线", L"ethernet", L"lan"}},
    {L"VPN", L"ms-settings:network-vpn", L"网络和 Internet", kColorNetwork,
     {L"虚拟专用网络", L"远程连接", L"内网"}},
    {L"代理", L"ms-settings:network-proxy", L"网络和 Internet", kColorNetwork,
     {L"代理服务器", L"系统代理", L"proxy"}},
    {L"飞行模式", L"ms-settings:network-airplanemode", L"网络和 Internet", kColorNetwork,
     {L"无线开关", L"飞行", L"airplane", L"mode"}},
    {L"移动热点", L"ms-settings:network-mobilehotspot", L"网络和 Internet", kColorNetwork,
     {L"热点", L"网络共享", L"hotspot", L"tethering"}},

    // 个性化
    {L"背景", L"ms-settings:personalization-background", L"个性化", kColorPersonalization,
     {L"壁纸", L"桌面背景", L"wallpaper", L"background"}},
    {L"颜色", L"ms-settings:colors", L"个性化", kColorPersonalization,
     {L"主题色", L"深色", L"浅色", L"accent", L"dark", L"light", L"color"}},
    {L"主题", L"ms-settings:themes", L"个性化", kColorPersonalization,
     {L"主题包", L"桌面主题", L"themes", L"theme"}},
    {L"锁屏界面", L"ms-settings:lockscreen", L"个性化", kColorPersonalization,
     {L"锁屏", L"登录背景", L"lock", L"screen"}},
    {L"任务栏", L"ms-settings:taskbar", L"个性化", kColorPersonalization,
     {L"图标", L"taskbar"}},
    {L"字体", L"ms-settings:fonts", L"个性化", kColorPersonalization,
     {L"安装字体", L"字体管理", L"fonts", L"font"}},

    // 应用
    {L"应用和功能", L"ms-settings:appsfeatures", L"应用", kColorApps,
     {L"卸载", L"卸载程序", L"软件卸载", L"已安装应用", L"程序列表", L"uninstall", L"installed",
      L"apps"}},
    {L"启动应用", L"ms-settings:startupapps", L"应用", kColorApps,
     {L"开机自启", L"自启动", L"启动项", L"开机启动", L"startup", L"autostart"}},
    {L"可选功能", L"ms-settings:optionalfeatures", L"应用", kColorApps,
     {L"可选", L"组件", L"optional", L"features"}},
    {L"默认应用", L"ms-settings:defaultapps", L"应用", kColorApps,
     {L"默认程序", L"打开方式", L"关联", L"default", L"apps", L"association"}},

    // 账户
    {L"账户信息", L"ms-settings:yourinfo", L"账户", kColorAccounts,
     {L"账户", L"个人资料", L"account", L"profile"}},
    {L"登录选项", L"ms-settings:signinoptions", L"账户", kColorAccounts,
     {L"登录", L"pin", L"指纹", L"人脸", L"密码", L"signin", L"fingerprint", L"password"}},
    {L"其他用户", L"ms-settings:otherusers", L"账户", kColorAccounts,
     {L"用户", L"家庭", L"添加账户", L"other", L"users", L"family"}},

    // 时间和语言
    {L"日期和时间", L"ms-settings:dateandtime", L"时间和语言", kColorTimeLanguage,
     {L"时间", L"时区", L"日期", L"time", L"date", L"timezone"}},
    {L"语言和区域", L"ms-settings:regionlanguage", L"时间和语言", kColorTimeLanguage,
     {L"语言", L"输入法", L"中文", L"区域", L"language", L"region", L"ime"}},
    {L"语音", L"ms-settings:speech", L"时间和语言", kColorTimeLanguage,
     {L"语音识别", L"听写", L"speech", L"voice"}},

    // 游戏
    {L"游戏模式", L"ms-settings:gaming-gamemode", L"游戏", kColorGaming,
     {L"游戏", L"性能", L"game", L"mode"}},

    // 隐私和安全性
    {L"隐私和安全性", L"ms-settings:privacy", L"隐私和安全性", kColorPrivacy,
     {L"隐私", L"权限", L"privacy", L"permissions"}},
    {L"麦克风权限", L"ms-settings:privacy-microphone", L"隐私和安全性", kColorPrivacy,
     {L"麦克风", L"权限", L"microphone", L"mic"}},
    {L"摄像头权限", L"ms-settings:privacy-webcam", L"隐私和安全性", kColorPrivacy,
     {L"摄像头", L"权限", L"camera", L"webcam"}},
    {L"位置权限", L"ms-settings:privacy-location", L"隐私和安全性", kColorPrivacy,
     {L"位置", L"定位", L"地理", L"location"}},

    // Windows 更新与安全
    {L"Windows 更新", L"ms-settings:windowsupdate", L"Windows 更新", kColorUpdateSecurity,
     {L"更新", L"升级", L"补丁", L"windows update"}},
    {L"更新历史记录", L"ms-settings:windowsupdate-history", L"Windows 更新", kColorUpdateSecurity,
     {L"更新记录", L"近期更新", L"history", L"recent", L"updates"}},
    {L"Windows 安全中心", L"ms-settings:windowsdefender", L"Windows 更新", kColorUpdateSecurity,
     {L"安全", L"病毒", L"防火墙", L"防护", L"defender", L"firewall", L"security"}},
    {L"恢复", L"ms-settings:recovery", L"Windows 更新", kColorUpdateSecurity,
     {L"重置", L"还原", L"重装", L"恢复出厂", L"recovery", L"reset"}},
    {L"激活", L"ms-settings:activation", L"Windows 更新", kColorUpdateSecurity,
     {L"密钥", L"许可证", L"activation", L"license", L"key"}},
    {L"疑难解答", L"ms-settings:troubleshoot", L"Windows 更新", kColorUpdateSecurity,
     {L"修复", L"troubleshoot", L"fix"}},
    {L"开发者选项", L"ms-settings:developers", L"Windows 更新", kColorUpdateSecurity,
     {L"开发者", L"调试", L"旁加载", L"developer", L"debug"}},

    // 辅助功能
    {L"辅助功能", L"ms-settings:easeofaccess", L"辅助功能", kColorAccessibility,
     {L"轻松使用", L"无障碍", L"放大镜", L"accessibility"}},
};

std::wstring GetEnv(const wchar_t* name) {
  wchar_t buffer[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
  return (length > 0 && length < ARRAYSIZE(buffer)) ? std::wstring(buffer, length) : std::wstring();
}

std::wstring FirstGlyph(const std::wstring& title) {
  if (title.empty()) return L"?";
  if (title.size() >= 2 && title[0] >= 0xD800 && title[0] <= 0xDBFF) return title.substr(0, 2);
  return title.substr(0, 1);
}

// SystemSettings.exe 的图标：所有设置页共用一枚齿轮，和开始菜单里的「设置」观感一致。
std::wstring SettingsIconKey() {
  static const std::wstring key = [] {
    const std::wstring root = GetEnv(L"SystemRoot");
    if (root.empty()) return std::wstring();
    const std::wstring exe = root + L"\\ImmersiveControlPanel\\SystemSettings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return std::wstring();
    return exe;
  }();
  return key;
}

}  // namespace

bool SettingsProtocolAvailable() {
  static const bool available = [] {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"ms-settings", 0, KEY_READ, &key) != ERROR_SUCCESS) {
      return false;
    }
    RegCloseKey(key);
    return true;
  }();
  return available;
}

std::vector<AppEntry> CollectSettingsPages() {
  std::vector<AppEntry> entries;
  if (!SettingsProtocolAvailable()) {
    LogWarn(L"未注册 ms-settings 协议，跳过系统设置页目录");
    return entries;
  }

  const std::wstring iconKey = SettingsIconKey();
  entries.reserve(std::size(kSettingsPages));
  for (const SettingsPageDef& def : kSettingsPages) {
    AppEntry entry;
    entry.title = def.title;
    entry.detail = std::wstring(L"设置 · ") + def.category;
    entry.launchTarget = def.uri;
    entry.iconKey = iconKey;
    entry.kind = LaunchKind::SettingsUri;
    entry.glyph = FirstGlyph(entry.title);
    entry.color = def.color;
    for (size_t i = 0; i < kMaxKeywords && def.keywords[i] != nullptr; ++i) {
      entry.keywords.emplace_back(def.keywords[i]);
    }
    entries.push_back(std::move(entry));
  }
  Log(L"系统设置页: %zu 条", entries.size());
  return entries;
}
