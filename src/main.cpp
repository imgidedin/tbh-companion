#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "generated_items.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

#ifndef TBH_DEVELOPMENT_MODE
#define TBH_DEVELOPMENT_MODE 1
#endif

constexpr int IDC_SERVER = 1001;
constexpr int IDC_TOKEN = 1002;
constexpr int IDC_STEAM = 1003;
constexpr int IDC_PAIRING_SECRET = 1004;
constexpr int IDC_OPEN = 1006;
constexpr int IDC_STATUS = 1007;
constexpr int IDC_AUTOSTART = 1009;
constexpr int IDC_EXPORT_DEV_RUNTIME = 1010;
constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;
constexpr int IDM_TRAY_OPEN = 2001;
constexpr int IDM_TRAY_WEB = 2002;
constexpr int IDM_TRAY_EXIT = 2003;
constexpr int IDM_TRAY_TOGGLE_TBH = 2004;
constexpr int IDM_TRAY_MINIMIZE_TO_TASKBAR = 2005;
constexpr int IDM_TRAY_EXPORT_DEV_RUNTIME = 2006;
constexpr bool kDevelopmentMode = TBH_DEVELOPMENT_MODE != 0;

constexpr wchar_t DEFAULT_SAVE_RELATIVE[] = L"\\AppData\\LocalLow\\TesseractStudio\\TaskbarHero\\SaveFile_Live.es3";
constexpr char DEFAULT_ES3_KEY[] = "emuMqG3bLYJ938ZDCfieWJ";
constexpr wchar_t STEAM_GAME_URI[] = L"steam://rungameid/3678970";
constexpr wchar_t AUTOSTART_VALUE_NAME[] = L"TBH Companion";
constexpr wchar_t APP_WINDOW_CLASS[] = L"TBHCompanionAgentWindow";
constexpr wchar_t SINGLE_INSTANCE_MUTEX_NAME[] = L"Local\\TBHCompanionAgentSingleInstance";

HINSTANCE g_instance = nullptr;
HWND g_server = nullptr;
HWND g_token = nullptr;
HWND g_steam = nullptr;
HWND g_pairing_secret = nullptr;
HWND g_autostart = nullptr;
HWND g_status = nullptr;
HWND g_main_window = nullptr;
HANDLE g_worker_stop = nullptr;
HANDLE g_worker_thread = nullptr;
NOTIFYICONDATAW g_tray{};
bool g_tray_added = false;
UINT g_taskbar_created_msg = 0;

std::string Utf8(const std::wstring& text);
std::wstring CompanionDir();
DWORD FindProcessIdByName(const wchar_t* process_name);

struct Config {
  std::wstring server = L"https://tbh.gided.in";
  std::wstring token;
  std::wstring steam_id;
  std::wstring pairing_secret;
  bool auto_start = false;
  bool minimize_to_taskbar = false;
};

Config LoadConfig();

struct SaveSummary {
  std::string steam_id;
  std::string owner_steam_id;
  std::string player_id;
  std::string version;
  long long current_stage_key = 0;
  long long current_stage_wave = 0;
  long long max_completed_stage = 0;
  long long arranged_pet_key = 0;
  long long gold = 0;
  std::string pets_json = "[]";
  std::string runes_json = "[]";
  std::string summary_json;
};

struct MemoryEvent {
  std::string type;
  std::string label;
  std::string difficulty;
  int seconds = 0;
  std::string progress;
  std::string item;
  std::string hero;
  std::string enemy;
  std::string clock;
  std::string color;
  std::string raw;
  std::string id;
  std::string category;  // categoria autoritativa: equipment/material/chest/craft/stage/death
  std::string grade;     // raridade autoritativa (EGradeType): COMMON..COSMIC, "" se nao se aplica
  int item_key = 0;      // chave do item em items.json (BoxOpenLog), 0 se nao se aplica
  int index = 0;
  long long ts = 0;    // epoch (s) de quando o agente viu o evento pela primeira vez
  int order_key = -1;  // janela de texto de origem no scan (chave transitoria)
};

int EventClockKey(const MemoryEvent& event) {
  if (event.clock.size() >= 4) {
    size_t colon = event.clock.find(':');
    if (colon != std::string::npos) {
      int hour = atoi(event.clock.substr(0, colon).c_str());
      int minute = atoi(event.clock.substr(colon + 1).c_str());
      return hour * 60 + minute;
    }
  }
  return -1;
}

struct MemorySnapshot {
  DWORD pid = 0;
  std::vector<MemoryEvent> events;
  std::string history_json = "null";
  std::string clears_json = "null";
  std::string watcher_json = "null";
};

struct MemoryRegion {
  unsigned char* base = nullptr;
  SIZE_T size = 0;
};

struct WorkerState {
  SaveSummary save;
  bool has_save = false;
  FILETIME save_mtime{};
  MemorySnapshot memory;
  std::vector<MemoryRegion> memory_regions;
  DWORD memory_pid = 0;
  DWORD last_memory_scan = 0;
  DWORD last_region_discovery = 0;
  std::string last_payload_hash;
  std::string last_sync_config_signature;
  std::set<std::string> memory_seen;     // ids ja vistos (historico + baseline)
  bool memory_baseline_ready = false;    // primeira leitura vira baseline e e ignorada
  long long synced_index = -1;     // maior index de evento confirmado pelo servidor
  std::string synced_first_id;     // detecta reset/reordenacao do historico local
};

std::wstring Trim(std::wstring value) {
  while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
  while (!value.empty() && iswspace(value.back())) value.pop_back();
  return value;
}

std::wstring GetWindowTextString(HWND hwnd) {
  int length = GetWindowTextLengthW(hwnd);
  std::wstring value(length, L'\0');
  if (length) GetWindowTextW(hwnd, value.data(), length + 1);
  return Trim(value);
}

void AddTrayIcon(HWND hwnd) {
  g_tray = {};
  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = hwnd;
  g_tray.uID = 1;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uCallbackMessage = WM_APP_TRAY;
  g_tray.hIcon = reinterpret_cast<HICON>(
      LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  wcsncpy_s(g_tray.szTip, L"TBH Companion", _TRUNCATE);
  g_tray_added = Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
}

void RemoveTrayIcon() {
  if (!g_tray_added) return;
  Shell_NotifyIconW(NIM_DELETE, &g_tray);
  g_tray_added = false;
}

void UpdateTrayTip(const std::wstring& text) {
  if (!g_tray_added) return;
  std::wstring tip = L"TBH Companion\n" + text;
  g_tray.uFlags = NIF_TIP;
  wcsncpy_s(g_tray.szTip, tip.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
  if (!g_tray_added) return;
  g_tray.uFlags = NIF_INFO;
  wcsncpy_s(g_tray.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(g_tray.szInfo, text.c_str(), _TRUNCATE);
  g_tray.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

struct GameWindowSearch {
  DWORD pid = 0;
  HWND visible = nullptr;
  HWND hidden = nullptr;
};

BOOL CALLBACK FindGameWindowProc(HWND window, LPARAM param) {
  auto* search = reinterpret_cast<GameWindowSearch*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid != search->pid) return TRUE;
  if (GetWindow(window, GW_OWNER)) return TRUE;
  LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  if (style & WS_CHILD) return TRUE;
  RECT rect{};
  if (!GetWindowRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
  if (IsWindowVisible(window)) {
    search->visible = window;
    return FALSE;
  }
  if (!search->hidden) search->hidden = window;
  return TRUE;
}

HWND FindGameWindow() {
  GameWindowSearch search;
  search.pid = FindProcessIdByName(L"TaskBarHero.exe");
  if (!search.pid) return nullptr;
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&search));
  return search.visible ? search.visible : search.hidden;
}

void ShowTrayMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  Config config = LoadConfig();
  HWND game_window = FindGameWindow();
  bool game_visible = game_window && IsWindowVisible(game_window);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Configurações");
  AppendMenuW(menu, MF_STRING, IDM_TRAY_WEB, L"Abrir painel web");
  AppendMenuW(menu, MF_STRING | (game_window ? 0 : MF_GRAYED), IDM_TRAY_TOGGLE_TBH,
              game_visible ? L"Esconder TBH" : L"Mostrar TBH");
  AppendMenuW(menu, MF_STRING | (config.minimize_to_taskbar ? MF_CHECKED : MF_UNCHECKED),
              IDM_TRAY_MINIMIZE_TO_TASKBAR, L"Minimizar para taskbar");
  if (kDevelopmentMode) {
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXPORT_DEV_RUNTIME, L"Atualizar save local");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Sair");
  SetMenuDefaultItem(menu, IDM_TRAY_OPEN, FALSE);
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

void SetStatus(const std::wstring& text) {
  if (g_status) {
    SetWindowTextW(g_status, text.c_str());
    InvalidateRect(g_status, nullptr, TRUE);
    UpdateWindow(g_status);
  }
  UpdateTrayTip(text);
}

void ToggleGameWindowVisibility() {
  HWND game_window = FindGameWindow();
  if (!game_window) {
    SetStatus(L"TaskBarHero.exe não encontrado.");
    return;
  }
  if (IsWindowVisible(game_window)) {
    ShowWindow(game_window, SW_HIDE);
    SetStatus(L"TBH escondido. O jogo continua rodando em segundo plano.");
  } else {
    ShowWindow(game_window, SW_RESTORE);
    SetForegroundWindow(game_window);
    SetStatus(L"TBH mostrado.");
  }
}

void PostStatus(const std::wstring& text) {
  std::wstring line = text + L"\r\n";
  std::wstring log_path = CompanionDir() + L"\\agent.log";
  HANDLE log = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log != INVALID_HANDLE_VALUE) {
    std::string utf8 = Utf8(line);
    DWORD written = 0;
    WriteFile(log, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(log);
  }
  if (!g_main_window) return;
  auto* copy = new std::wstring(text);
  PostMessageW(g_main_window, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(copy));
}

bool SameFileTime(const FILETIME& a, const FILETIME& b) {
  return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}

bool FileMTime(const std::wstring& path, FILETIME& out) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
  out = data.ftLastWriteTime;
  return true;
}

std::string Fnv1aHash(const std::string& text) {
  unsigned long long hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char buffer[32]{};
  sprintf_s(buffer, "%016llx", hash);
  return buffer;
}

std::string HexLower(const std::vector<unsigned char>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    out.push_back(digits[(byte >> 4) & 0x0F]);
    out.push_back(digits[byte & 0x0F]);
  }
  return out;
}

std::string Sha256Hex(const std::string& text) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_length = 0;
  DWORD data_length = 0;
  std::vector<unsigned char> hash_object;
  std::vector<unsigned char> digest(32);

  NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status >= 0) {
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
                               &data_length, 0);
  }
  if (status >= 0) {
    hash_object.resize(object_length);
    status = BCryptCreateHash(alg, &hash, hash_object.data(), object_length, nullptr, 0, 0);
  }
  if (status >= 0) {
    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
                            static_cast<ULONG>(text.size()), 0);
  }
  if (status >= 0) {
    status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
  }
  if (hash) BCryptDestroyHash(hash);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  return status >= 0 ? HexLower(digest) : "";
}

std::string PairingHash(const Config& config, const std::string& steam_id) {
  std::string secret = Utf8(config.pairing_secret);
  if (secret.empty() || steam_id.empty()) return "";
  return Sha256Hex("TBH Companion Pairing v1:" + steam_id + ":" + secret);
}

std::string EffectiveSteamId(const Config& config, const WorkerState& state) {
  std::string steam = Utf8(config.steam_id);
  if (steam.empty() && !state.save.steam_id.empty()) steam = state.save.steam_id;
  return steam.empty() ? "default" : steam;
}

std::string SyncConfigSignature(const Config& config, const WorkerState& state) {
  std::string steam = EffectiveSteamId(config, state);
  return Fnv1aHash(Utf8(config.server) + "|" + Utf8(config.token) + "|" + steam + "|" + PairingHash(config, steam));
}

std::wstring ExePath();

std::wstring ConfigPath() {
  wchar_t local_app_data[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  std::wstring dir = length ? std::wstring(local_app_data, length) : L".";
  dir += L"\\TBH Companion";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\config.ini";
}

std::wstring CompanionDir() {
  wchar_t local_app_data[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  std::wstring dir = length ? std::wstring(local_app_data, length) : L".";
  dir += L"\\TBH Companion";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring AutoStartCommand() {
  return L"\"" + ExePath() + L"\"";
}

bool IsAutoStartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t value[MAX_PATH * 2]{};
  DWORD type = 0;
  DWORD size = sizeof(value);
  LONG result = RegQueryValueExW(key, AUTOSTART_VALUE_NAME, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || type != REG_SZ) return false;
  return Trim(value) == AutoStartCommand();
}

bool SetAutoStart(bool enabled) {
  HKEY key = nullptr;
  LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                                KEY_SET_VALUE, nullptr, &key, nullptr);
  if (result != ERROR_SUCCESS) return false;
  if (enabled) {
    std::wstring command = AutoStartCommand();
    result = RegSetValueExW(key, AUTOSTART_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else {
    result = RegDeleteValueW(key, AUTOSTART_VALUE_NAME);
    if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

Config LoadConfig() {
  Config config;
  std::wstring path = ConfigPath();
  wchar_t buffer[4096]{};

  GetPrivateProfileStringW(L"server", L"url", config.server.c_str(), buffer, 4096, path.c_str());
  config.server = Trim(buffer);

  GetPrivateProfileStringW(L"server", L"token", L"", buffer, 4096, path.c_str());
  config.token = Trim(buffer);

  GetPrivateProfileStringW(L"player", L"steam_id", L"", buffer, 4096, path.c_str());
  config.steam_id = Trim(buffer);

  GetPrivateProfileStringW(L"player", L"pairing_secret", L"", buffer, 4096, path.c_str());
  config.pairing_secret = Trim(buffer);

  config.auto_start = IsAutoStartEnabled();
  config.minimize_to_taskbar = GetPrivateProfileIntW(L"app", L"minimize_to_taskbar", 0, path.c_str()) != 0;

  return config;
}

void SaveConfig(const Config& config) {
  std::wstring path = ConfigPath();
  WritePrivateProfileStringW(L"server", L"url", config.server.c_str(), path.c_str());
  WritePrivateProfileStringW(L"server", L"token", config.token.c_str(), path.c_str());
  WritePrivateProfileStringW(L"player", L"steam_id", config.steam_id.c_str(), path.c_str());
  WritePrivateProfileStringW(L"player", L"pairing_secret", config.pairing_secret.c_str(), path.c_str());
  WritePrivateProfileStringW(L"app", L"auto_start", config.auto_start ? L"1" : L"0", path.c_str());
  WritePrivateProfileStringW(L"app", L"minimize_to_taskbar", config.minimize_to_taskbar ? L"1" : L"0", path.c_str());
  SetAutoStart(config.auto_start);
}

Config ReadConfigFromUi() {
  Config config;
  config.server = GetWindowTextString(g_server);
  config.token = GetWindowTextString(g_token);
  config.steam_id = GetWindowTextString(g_steam);
  config.pairing_secret = GetWindowTextString(g_pairing_secret);
  config.auto_start = g_autostart && SendMessageW(g_autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
  config.minimize_to_taskbar = LoadConfig().minimize_to_taskbar;
  return config;
}

bool UiConfigReady() {
  return g_server && g_token && g_steam && g_pairing_secret && g_autostart;
}

void SaveConfigFromUi() {
  if (!UiConfigReady()) return;
  SaveConfig(ReadConfigFromUi());
}

std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) return {};
  int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring out(size > 0 ? size - 1 : 0, L'\0');
  if (size > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
  return out;
}

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  std::string number;
  std::string string;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  static JsonValue Null() { return {}; }
  static JsonValue Bool(bool value) {
    JsonValue out;
    out.type = Type::Bool;
    out.boolean = value;
    return out;
  }
  static JsonValue Number(std::string value) {
    JsonValue out;
    out.type = Type::Number;
    out.number = value.empty() ? "0" : std::move(value);
    return out;
  }
  static JsonValue Number(long long value) { return Number(std::to_string(value)); }
  static JsonValue Number(double value) {
    if (std::fabs(value - std::round(value)) < 0.000000001) return Number(static_cast<long long>(std::llround(value)));
    std::ostringstream stream;
    stream.precision(15);
    stream << value;
    return Number(stream.str());
  }
  static JsonValue String(std::string value) {
    JsonValue out;
    out.type = Type::String;
    out.string = std::move(value);
    return out;
  }
  static JsonValue Array() {
    JsonValue out;
    out.type = Type::Array;
    return out;
  }
  static JsonValue Object() {
    JsonValue out;
    out.type = Type::Object;
    return out;
  }
};

void ObjectSet(JsonValue& object, const std::string& key, JsonValue value) {
  object.object.push_back({key, std::move(value)});
}

const JsonValue* ObjectGet(const JsonValue& object, const std::string& key) {
  if (object.type != JsonValue::Type::Object) return nullptr;
  for (const auto& item : object.object) {
    if (item.first == key) return &item.second;
  }
  return nullptr;
}

JsonValue* ObjectGet(JsonValue& object, const std::string& key) {
  if (object.type != JsonValue::Type::Object) return nullptr;
  for (auto& item : object.object) {
    if (item.first == key) return &item.second;
  }
  return nullptr;
}

bool JsonBool(const JsonValue* value, bool fallback = false) {
  if (!value) return fallback;
  if (value->type == JsonValue::Type::Bool) return value->boolean;
  if (value->type == JsonValue::Type::Number) return value->number != "0";
  return fallback;
}

std::string JsonStringValue(const JsonValue* value) {
  if (!value) return {};
  if (value->type == JsonValue::Type::String) return value->string;
  if (value->type == JsonValue::Type::Number) return value->number;
  return {};
}

std::string JsonNumberKey(const JsonValue* value) {
  std::string text = JsonStringValue(value);
  size_t dot = text.find('.');
  if (dot != std::string::npos) text.resize(dot);
  return text;
}

double JsonNumberDouble(const JsonValue* value, double fallback = 0) {
  if (!value || value->type != JsonValue::Type::Number) return fallback;
  try {
    return std::stod(value->number);
  } catch (...) {
    return fallback;
  }
}

bool IsZeroNumber(const JsonValue* value) {
  return value && value->type == JsonValue::Type::Number && JsonNumberDouble(value) == 0;
}

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool Parse(JsonValue& out) {
    pos_ = 0;
    if (!ParseValue(out)) return false;
    SkipWs();
    return pos_ == text_.size();
  }

 private:
  void SkipWs() {
    while (pos_ < text_.size() && isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool Consume(char expected) {
    SkipWs();
    if (pos_ >= text_.size() || text_[pos_] != expected) return false;
    ++pos_;
    return true;
  }

  bool ParseValue(JsonValue& out) {
    SkipWs();
    if (pos_ >= text_.size()) return false;
    char c = text_[pos_];
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == '"') {
      std::string value;
      if (!ParseString(value)) return false;
      out = JsonValue::String(value);
      return true;
    }
    if (c == 't' && Match("true")) {
      out = JsonValue::Bool(true);
      return true;
    }
    if (c == 'f' && Match("false")) {
      out = JsonValue::Bool(false);
      return true;
    }
    if (c == 'n' && Match("null")) {
      out = JsonValue::Null();
      return true;
    }
    return ParseNumber(out);
  }

  bool Match(const char* literal) {
    size_t length = strlen(literal);
    if (text_.compare(pos_, length, literal) != 0) return false;
    pos_ += length;
    return true;
  }

  bool ParseString(std::string& out) {
    if (!Consume('"')) return false;
    out.clear();
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) return false;
      char escaped = text_[pos_++];
      switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > text_.size()) return false;
          unsigned int code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code += h - '0';
            else if (h >= 'a' && h <= 'f') code += h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') code += h - 'A' + 10;
            else return false;
          }
          if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
          } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseNumber(JsonValue& out) {
    SkipWs();
    size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (start == pos_) return false;
    out = JsonValue::Number(text_.substr(start, pos_ - start));
    return true;
  }

  bool ParseArray(JsonValue& out) {
    if (!Consume('[')) return false;
    out = JsonValue::Array();
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JsonValue value;
      if (!ParseValue(value)) return false;
      out.array.push_back(std::move(value));
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  bool ParseObject(JsonValue& out) {
    if (!Consume('{')) return false;
    out = JsonValue::Object();
    SkipWs();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      std::string key;
      if (!ParseString(key)) return false;
      if (!Consume(':')) return false;
      JsonValue value;
      if (!ParseValue(value)) return false;
      out.object.push_back({std::move(key), std::move(value)});
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      if (!Consume(',')) return false;
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
};

std::string JsonEscape(const std::string& input);

std::string JsonSerialize(const JsonValue& value) {
  switch (value.type) {
    case JsonValue::Type::Null:
      return "null";
    case JsonValue::Type::Bool:
      return value.boolean ? "true" : "false";
    case JsonValue::Type::Number:
      return value.number.empty() ? "0" : value.number;
    case JsonValue::Type::String:
      return "\"" + JsonEscape(value.string) + "\"";
    case JsonValue::Type::Array: {
      std::string out = "[";
      for (size_t i = 0; i < value.array.size(); ++i) {
        if (i) out += ",";
        out += JsonSerialize(value.array[i]);
      }
      out += "]";
      return out;
    }
    case JsonValue::Type::Object: {
      std::string out = "{";
      for (size_t i = 0; i < value.object.size(); ++i) {
        if (i) out += ",";
        out += "\"" + JsonEscape(value.object[i].first) + "\":" + JsonSerialize(value.object[i].second);
      }
      out += "}";
      return out;
    }
  }
  return "null";
}

bool ParseJson(const std::string& text, JsonValue& out) {
  return JsonParser(text).Parse(out);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::wstring LowerWide(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return value;
}

std::wstring NormalizeMemoryText(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size());
  bool spaced = false;
  for (wchar_t c : text) {
    bool is_private = c >= 0xE000 && c <= 0xF8FF;
    bool is_control = (c >= 0 && c <= 0x08) || c == 0x0B || c == 0x0C || (c >= 0x0E && c <= 0x1F);
    if (is_private || is_control || iswspace(c)) {
      if (!spaced && !out.empty()) {
        out.push_back(L' ');
        spaced = true;
      }
      continue;
    }
    out.push_back(c);
    spaced = false;
  }
  while (!out.empty() && out.back() == L' ') out.pop_back();
  return out;
}

std::string CleanMarkupUtf8(const std::wstring& input) {
  std::wstring out;
  out.reserve(input.size());
  bool in_tag = false;
  for (wchar_t c : input) {
    if (c == L'<') {
      in_tag = true;
      continue;
    }
    if (c == L'>') {
      in_tag = false;
      continue;
    }
    if (!in_tag) out.push_back(c);
  }
  return Utf8(NormalizeMemoryText(out));
}

// Cor da primeira tag <color=#RRGGBB> (raridade no frontend), em maiusculas.
std::string ExtractColorTag(const std::wstring& text) {
  size_t pos = text.find(L"<color=#");
  if (pos == std::wstring::npos) return "";
  size_t start = pos + 7;  // aponta para '#'
  size_t end = text.find(L'>', start);
  if (end == std::wstring::npos || end - start != 7) return "";
  std::string out = Utf8(text.substr(start, 7));
  for (char& c : out) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  return out;
}

// Texto limpo do evento: sem tags de markup e sem o sufixo de relogio
// "[HH:MM]" (que ja vai no campo clock).
std::string CleanEventRaw(const std::wstring& input) {
  std::wstring out;
  out.reserve(input.size());
  bool in_tag = false;
  for (wchar_t c : input) {
    if (c == L'<') { in_tag = true; continue; }
    if (c == L'>') { in_tag = false; continue; }
    if (!in_tag) out.push_back(c);
  }
  out = NormalizeMemoryText(out);
  size_t bracket = out.rfind(L'[');
  if (bracket != std::wstring::npos && out.size() - bracket == 7 && out.back() == L']' &&
      iswdigit(out[bracket + 1]) && iswdigit(out[bracket + 2]) && out[bracket + 3] == L':' &&
      iswdigit(out[bracket + 4]) && iswdigit(out[bracket + 5])) {
    out.erase(bracket);
    while (!out.empty() && out.back() == L' ') out.pop_back();
  }
  return Utf8(out);
}

// Texto interno do primeiro <color=#...>...</color> (nome do item principal nos
// logs de cubo), com markup interno removido.
std::string FirstColoredText(const std::wstring& text) {
  size_t open = text.find(L"<color=#");
  if (open == std::wstring::npos) return "";
  size_t gt = text.find(L'>', open);
  if (gt == std::wstring::npos) return "";
  size_t close = text.find(L"</color>", gt);
  if (close == std::wstring::npos) return "";
  return CleanMarkupUtf8(text.substr(gt + 1, close - gt - 1));
}

std::string DropItemNameFromRaw(const std::string& raw) {
  size_t obtained = raw.find(" obtido");
  if (obtained == std::string::npos) return raw;
  std::string item = raw.substr(0, obtained);
  while (!item.empty() && item.back() == ' ') item.pop_back();
  return item;
}

// Relogio "HH:MM" extraido do sufixo " <voffset...>[HH:MM]...".
std::string ExtractClockSuffix(const std::wstring& text) {
  size_t rb = text.rfind(L']');
  if (rb == std::wstring::npos) return "";
  size_t lb = text.rfind(L'[', rb);
  if (lb == std::wstring::npos || rb <= lb) return "";
  std::wstring inner = text.substr(lb + 1, rb - lb - 1);
  return (inner.size() >= 4 && inner.find(L':') != std::wstring::npos) ? Utf8(inner) : "";
}

std::string EventId(const MemoryEvent& event) {
  if (event.type == "clear") return "clear|" + event.label + "|" + std::to_string(event.seconds) + "|" + event.clock;
  if (event.type == "failure") return "failure|" + event.label + "|" + event.progress + "|" + event.clock;
  if (event.type == "death") return "death|" + event.hero + "|" + event.enemy + "|" + event.clock;
  if (event.type == "craft") return "craft|" + event.category + "|" + event.item + "|" + event.clock;
  if (event.type == "drop") {
    bool craft = event.raw.rfind("Resultado", 0) == 0;
    return std::string(craft ? "craft|" : "drop|") + event.item + "|" + event.clock;
  }
  return event.type + "|" + event.raw;
}

std::vector<unsigned char> Utf16Needle(const wchar_t* text) {
  std::vector<unsigned char> bytes;
  for (const wchar_t* p = text; *p; ++p) {
    wchar_t c = *p;
    bytes.push_back(static_cast<unsigned char>(c & 0xFF));
    bytes.push_back(static_cast<unsigned char>((c >> 8) & 0xFF));
  }
  return bytes;
}

std::wstring DecodeUtf16Le(const unsigned char* data, size_t size) {
  std::wstring out;
  out.reserve(size / 2);
  for (size_t i = 0; i + 1 < size; i += 2) {
    wchar_t c = static_cast<wchar_t>(data[i] | (data[i + 1] << 8));
    out.push_back(c);
  }
  return out;
}

constexpr unsigned kEventClear = 1u;
constexpr unsigned kEventFailure = 2u;
constexpr unsigned kEventDeath = 4u;
constexpr unsigned kEventDrop = 8u;
constexpr unsigned kEventAll = kEventClear | kEventFailure | kEventDeath | kEventDrop;

struct LogNeedle {
  std::vector<unsigned char> bytes;
  unsigned mask;
};

const std::vector<LogNeedle>& LogNeedles() {
  static const std::vector<LogNeedle> needles = {
      {Utf16Needle(L"concluído"), kEventClear},
      {Utf16Needle(L"concluido"), kEventClear},
      {Utf16Needle(L"Falha ao concluir"), kEventFailure},
      {Utf16Needle(L"derrotado"), kEventDeath},
      {Utf16Needle(L"obtido"), kEventDrop},
      {Utf16Needle(L"Resultado de fabricação"), kEventDrop},
  };
  return needles;
}

struct NeedleHit {
  size_t index;
  unsigned mask;
};

// Single-purpose fast multi-pattern search: memchr (SIMD na CRT) localiza o
// primeiro byte e memcmp confirma a needle completa. Muito mais rapido que
// std::search byte a byte.
void FindNeedleHits(const unsigned char* data, size_t size, std::vector<NeedleHit>& hits) {
  hits.clear();
  for (const auto& needle : LogNeedles()) {
    const size_t n = needle.bytes.size();
    if (n == 0 || n > size) continue;
    const unsigned char first = needle.bytes[0];
    const size_t last_start = size - n;
    size_t pos = 0;
    while (pos <= last_start) {
      const void* found = memchr(data + pos, first, last_start - pos + 1);
      if (!found) break;
      size_t index = static_cast<size_t>(static_cast<const unsigned char*>(found) - data);
      if (memcmp(data + index, needle.bytes.data(), n) == 0) {
        hits.push_back({index, needle.mask});
        pos = index + n;
      } else {
        pos = index + 1;
      }
    }
  }
  std::sort(hits.begin(), hits.end(), [](const NeedleHit& a, const NeedleHit& b) { return a.index < b.index; });
}

DWORD FindProcessIdByName(const wchar_t* process_name) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD found = 0;
  std::wstring wanted = LowerWide(process_name);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (LowerWide(entry.szExeFile) == wanted) {
        found = entry.th32ProcessID;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

bool IsReadableRegion(const MEMORY_BASIC_INFORMATION& info) {
  if (info.State != MEM_COMMIT) return false;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  DWORD protect = info.Protect & 0xFF;
  return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
         protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

std::vector<unsigned char> ReadProcessBytes(HANDLE process, const unsigned char* address, SIZE_T size) {
  std::vector<unsigned char> bytes(size);
  SIZE_T read = 0;
  if (!ReadProcessMemory(process, address, bytes.data(), size, &read) || read == 0) return {};
  bytes.resize(read);
  return bytes;
}

// ===== Leitura direta do LogManager via mapa IL2CPP =====
// O bloco abaixo entre os marcadores BEGIN/END e GERADO automaticamente por
// scripts/refresh_il2cpp_map.py (Il2CppDumper + verificacao na memoria viva).
// Quando o jogo atualizar, rode esse script e recompile. NAO edite a mao.
//
//   nn<LogManager>_TypeInfo : RVA do singleton base do LogManager
//   StaticFieldsOffset      : Il2CppClass.static_fields (depende da versao do il2cpp)
//   LogManagerListOffset    : LogManager.<List<LogData>> (ordem de insercao)
//   LogDataTextOffset       : LogData.<texto> (string com markup do evento)
//   LogDataClockOffset      : LogData.<relogio> (sufixo " <voffset...>[HH:MM]...")
//   LogDataDateTimeOffset   : LogData.<DateTime> (ticks .NET, hora local)
//   BoxOpenItemKeyOffset    : BoxOpenLog.<itemStringKey> ("ItemName_<key>")
//   BoxOpenGradeOffset      : BoxOpenLog.<EGradeType> (raridade real do drop)
//   kGradeNames             : EGradeType -> nome canonico (frontend mapeia p/ PT)
//
// ===== BEGIN IL2CPP MAP (TaskBarHero 1.00.13) =====
constexpr uintptr_t kLogManagerTypeInfoRva = 0x5E0CBC8;
constexpr uintptr_t kKlassStaticFieldsOffset = 0xB8;
constexpr uintptr_t kLogManagerListOffset = 0x20;
constexpr uintptr_t kLogDataTextOffset = 0x20;
constexpr uintptr_t kLogDataClockOffset = 0x28;
constexpr uintptr_t kLogDataDateTimeOffset = 0x30;
constexpr uintptr_t kBoxOpenItemKeyOffset = 0x40;
constexpr uintptr_t kBoxOpenGradeOffset = 0x48;
static const char* const kGradeNames[] = {
    "COMMON", "UNCOMMON", "RARE", "LEGENDARY", "IMMORTAL",
    "ARCANA", "BEYOND", "CELESTIAL", "DIVINE", "COSMIC",
};
// ===== END IL2CPP MAP =====

uintptr_t FindModuleBase(DWORD pid, const wchar_t* module_name) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  uintptr_t base = 0;
  std::wstring wanted = LowerWide(module_name);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      if (LowerWide(entry.szModule) == wanted) {
        base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        break;
      }
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return base;
}

uintptr_t ReadPtr(HANDLE process, uintptr_t address) {
  uintptr_t value = 0;
  SIZE_T read = 0;
  if (!address) return 0;
  if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value, sizeof(value), &read) ||
      read != sizeof(value)) {
    return 0;
  }
  return value;
}

bool ReadInt32(HANDLE process, uintptr_t address, int& value) {
  SIZE_T read = 0;
  return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value, sizeof(value), &read) &&
         read == sizeof(value);
}

// System.String: comprimento (int32) @0x10, chars UTF-16 @0x14.
std::wstring ReadManagedString(HANDLE process, uintptr_t address) {
  if (!address) return L"";
  int length = 0;
  if (!ReadInt32(process, address + 0x10, length) || length <= 0 || length > 8192) return L"";
  std::wstring out(static_cast<size_t>(length), L'\0');
  SIZE_T read = 0;
  if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + 0x14), &out[0],
                         static_cast<SIZE_T>(length) * 2, &read)) {
    return L"";
  }
  out.resize(read / 2);
  return out;
}

// Ticks .NET (hora local do jogo) -> epoch UTC em segundos.
long long DotNetTicksToEpoch(unsigned long long raw_ticks) {
  unsigned long long ticks = raw_ticks & 0x3FFFFFFFFFFFFFFFULL;  // remove o DateTimeKind
  long long as_if_utc = static_cast<long long>(ticks / 10000000ULL) - 62135596800LL;
  struct tm parts {};
  time_t base = static_cast<time_t>(as_if_utc);
  if (gmtime_s(&parts, &base) != 0) return as_if_utc;
  parts.tm_isdst = -1;
  time_t utc = mktime(&parts);  // interpreta os campos como hora local
  return utc > 0 ? static_cast<long long>(utc) : as_if_utc;
}

void AppendRegexEvents(std::vector<MemoryEvent>& events, const std::wstring& text, unsigned mask = kEventAll) {
  static const std::wregex clear_re(
      LR"((?:<color=#[0-9A-Fa-f]+>)?Est[aá]gio\s+(\d+-\d+)(?:</color>)?\s+conclu[ií]do\.\s*\((\d+)s\)(?:\s*<voffset[^>]*><size=[^>]*>\[([^\]]+)\]</size></voffset>)?)",
      std::regex_constants::icase);
  static const std::wregex fail_re(
      LR"(Falha ao concluir\s+(?:<color=#[0-9A-Fa-f]+>)?Est[aá]gio\s+(\d+-\d+)(?:</color>)?\.\s*\((\d+/\d+)\)(?:\s*<voffset[^>]*><size=[^>]*>\[([^\]]+)\]</size></voffset>)?)",
      std::regex_constants::icase);
  static const std::wregex death_re(
      LR"((?:<color=#[0-9A-Fa-f]+>)?([^<>()]{2,40})(?:</color>)?\s+foi derrotado\.\s*\(([^)]+)\)(?:\s*<voffset[^>]*><size=[^>]*>\[([^\]]+)\]</size></voffset>)?)",
      std::regex_constants::icase);
  static const std::wregex drop_re(
      LR"((?:(?:Resultado de fabricação:\s*(?:<color=#[0-9A-Fa-f]+>)?([^<.]{2,80})(?:</color>)?)|(?:<color=#[0-9A-Fa-f]+>)?([A-ZÁÉÍÓÚÃÕÂÊÔÇ][A-Za-zÁÉÍÓÚáéíóúÃÕãõÂÊÔâêôÇç0-9'’+ -]{1,80})(?:</color>)?\s+obtido)\.(?:\s*<voffset[^>]*><size=[^>]*>\[([^\]]+)\]</size></voffset>)?)");

  std::vector<std::pair<size_t, MemoryEvent>> found;
  auto add_matches = [&](const std::wregex& regex, const std::string& type) {
    for (std::wsregex_iterator it(text.begin(), text.end(), regex), end; it != end; ++it) {
      const std::wsmatch& match = *it;
      MemoryEvent event;
      event.type = type;
      event.color = ExtractColorTag(match.str(0));
      event.raw = CleanEventRaw(match.str(0));
      if (type == "clear") {
        event.label = Utf8(match.str(1));
        event.seconds = _wtoi(match.str(2).c_str());
        event.clock = match.size() > 3 ? Utf8(match.str(3)) : "";
      } else if (type == "failure") {
        event.label = Utf8(match.str(1));
        event.progress = Utf8(match.str(2));
        event.clock = match.size() > 3 ? Utf8(match.str(3)) : "";
      } else if (type == "death") {
        event.hero = CleanMarkupUtf8(match.str(1));
        event.enemy = CleanMarkupUtf8(match.str(2));
        event.clock = match.size() > 3 ? Utf8(match.str(3)) : "";
      } else if (type == "drop") {
        std::wstring item = match.str(1).empty() ? match.str(2) : match.str(1);
        event.item = CleanMarkupUtf8(item);
        event.clock = match.size() > 3 ? Utf8(match.str(3)) : "";
        std::string lower = LowerAscii(event.item);
        if (lower.rfind("falha ao concluir", 0) == 0 || lower.rfind("estágio", 0) == 0 || lower.rfind("estagio", 0) == 0) continue;
        if (lower.find("certificado") != std::string::npos || lower.find("descritor") != std::string::npos ||
            lower.find("monitor") != std::string::npos || lower.find("vers") != std::string::npos ||
            lower.find("não pôde") != std::string::npos) {
          continue;
        }
      }
      event.id = EventId(event);
      found.emplace_back(static_cast<size_t>(match.position(0)), std::move(event));
    }
  };

  if (mask & kEventClear) add_matches(clear_re, "clear");
  if (mask & kEventFailure) add_matches(fail_re, "failure");
  if (mask & kEventDeath) add_matches(death_re, "death");
  if (mask & kEventDrop) add_matches(drop_re, "drop");

  // O buffer de log do jogo e cronologico: ordena pela posicao no texto para
  // intercalar corretamente os tipos de evento.
  std::stable_sort(found.begin(), found.end(),
                   [](const std::pair<size_t, MemoryEvent>& a, const std::pair<size_t, MemoryEvent>& b) {
                     return a.first < b.first;
                   });

  for (auto& item : found) events.push_back(std::move(item.second));
}

std::vector<MemoryEvent> UniqueEvents(const std::vector<MemoryEvent>& events) {
  std::set<std::string> seen;
  std::vector<MemoryEvent> output;
  for (const auto& event : events) {
    std::string id = event.id.empty() ? EventId(event) : event.id;
    if (seen.count(id)) continue;
    seen.insert(id);
    MemoryEvent copy = event;
    copy.id = id;
    copy.index = static_cast<int>(output.size());
    output.push_back(std::move(copy));
  }
  return output;
}

// Le o nome da classe IL2CPP de um objeto gerenciado (obj -> Il2CppClass -> name).
std::string ReadObjectClassName(HANDLE process, uintptr_t obj) {
  uintptr_t klass = ReadPtr(process, obj) & ~uintptr_t(1);
  uintptr_t name_ptr = ReadPtr(process, klass + 0x10);
  if (!name_ptr) return "";
  char buffer[96] = {0};
  SIZE_T read = 0;
  if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(name_ptr), buffer, sizeof(buffer) - 1, &read)) {
    return "";
  }
  return std::string(buffer, strnlen(buffer, sizeof(buffer) - 1));
}

// EGradeType -> nome canonico (mapeado para PT no frontend). Indexa kGradeNames,
// que e regenerado pelo refresh_il2cpp_map.py a partir do enum do jogo.
const char* GradeTypeName(int grade) {
  if (grade < 0 || grade >= static_cast<int>(sizeof(kGradeNames) / sizeof(kGradeNames[0]))) return "";
  return kGradeNames[grade];
}

const JsonValue* ItemByKey(const std::string& key);  // definido adiante

// Categoria a partir do "type" do items.json (GEAR/MATERIAL/STAGEBOX).
std::string CategoryFromItemType(const std::string& item_type) {
  if (item_type == "GEAR") return "equipment";
  if (item_type == "MATERIAL") return "material";
  if (item_type == "STAGEBOX") return "chest";
  return "";
}

// Familia de logs de cubo -> categoria do evento (type "craft"). Vazio se a
// classe nao for de cubo. Todas tem item key @0x40 e grade @0x48 (como
// BoxOpenLog), exceto a sintese (so dois EGradeType @0x40/@0x44).
std::string CubeLogCategory(const std::string& klass) {
  if (klass == "CubeCraftingLog" || klass == "CubeAlchemyLog" ||
      klass == "CubeExtractionLog" || klass == "CubeOfferingLog") {
    return "craft";
  }
  if (klass == "CubeSynthesisLog") return "synthesis";
  if (klass == "CubeDecorationLog" || klass == "CubeEngravingLog" ||
      klass == "CubeInscriptionLog") {
    return "decoration";
  }
  return "";
}

// Le a List<LogData> do LogManager do jogo via cadeia de ponteiros do mapa
// IL2CPP. Os eventos vem em ordem de insercao (cronologica) e com o DateTime
// real de cada um — sem varrer heap, sem adivinhar ordem.
// Retorna false se a cadeia nao resolver (jogo carregando ou versao diferente).
bool ReadLogManagerEvents(DWORD pid, std::vector<MemoryEvent>& out, std::string* error = nullptr) {
  out.clear();
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (!pid) return fail("jogo nao esta rodando");
  uintptr_t module_base = FindModuleBase(pid, L"GameAssembly.dll");
  if (!module_base) return fail("GameAssembly.dll nao encontrada");
  HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!process) return fail("sem acesso ao processo");

  bool ok = false;
  do {
    uintptr_t klass = ReadPtr(process, module_base + kLogManagerTypeInfoRva);
    if (!klass) { fail("TypeInfo nulo (jogo inicializando?)"); break; }
    uintptr_t statics = ReadPtr(process, klass + kKlassStaticFieldsOffset);
    uintptr_t instance = ReadPtr(process, statics);
    if (!instance) { fail("singleton LogManager ainda nao criado"); break; }
    uintptr_t list = ReadPtr(process, instance + kLogManagerListOffset);
    if (!list) { fail("lista de log nula"); break; }
    uintptr_t items = ReadPtr(process, list + 0x10);  // List<T>._items (array)
    int size = 0;
    if (!items || !ReadInt32(process, list + 0x18, size) || size < 0 || size > 4000) {
      fail("lista de log invalida");
      break;
    }
    // Le todos os ponteiros do array de uma vez.
    std::vector<uintptr_t> pointers(static_cast<size_t>(size));
    SIZE_T read = 0;
    if (size > 0 && (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(items + 0x20), pointers.data(),
                                        pointers.size() * sizeof(uintptr_t), &read) ||
                     read != pointers.size() * sizeof(uintptr_t))) {
      fail("falha lendo array de log");
      break;
    }
    out.reserve(pointers.size());
    for (uintptr_t obj : pointers) {
      if (!obj) continue;
      std::wstring text = ReadManagedString(process, ReadPtr(process, obj + kLogDataTextOffset));
      if (text.empty()) continue;
      std::wstring clock_suffix = ReadManagedString(process, ReadPtr(process, obj + kLogDataClockOffset));
      unsigned long long ticks = 0;
      ReadProcessMemory(process, reinterpret_cast<LPCVOID>(obj + kLogDataDateTimeOffset), &ticks,
                        sizeof(ticks), &read);
      std::string klass_name = ReadObjectClassName(process, obj);
      long long stamp = ticks ? DotNetTicksToEpoch(ticks) : 0;

      // Familia de cubo (fabricacao/sintese/decoracao/...): o texto nao casa com
      // os regexes de evento, entao montamos direto a partir da classe IL2CPP +
      // campos estruturados (item key @0x40, grade @0x48; sintese usa @0x44).
      std::string cube_category = CubeLogCategory(klass_name);
      if (!cube_category.empty()) {
        std::wstring full = text + clock_suffix;
        MemoryEvent event;
        event.type = "craft";
        event.category = cube_category;
        event.color = ExtractColorTag(full);
        event.raw = CleanEventRaw(full);
        event.clock = ExtractClockSuffix(clock_suffix);
        event.ts = stamp;
        int grade_value = 10;
        if (klass_name == "CubeSynthesisLog") {
          ReadInt32(process, obj + 0x44, grade_value);  // afterGrade (grau resultante)
          // Sintese nao tem item; o titulo usa o texto cru ("Grau X consumido...").
        } else {
          event.item = FirstColoredText(text);  // nome principal (item fabricado/decorado)
          std::wstring item_key_str = ReadManagedString(process, ReadPtr(process, obj + kBoxOpenItemKeyOffset));
          size_t underscore = item_key_str.rfind(L'_');
          if (underscore != std::wstring::npos) {
            event.item_key = atoi(Utf8(item_key_str.substr(underscore + 1)).c_str());
          }
          ReadInt32(process, obj + kBoxOpenGradeOffset, grade_value);
        }
        event.grade = GradeTypeName(grade_value);
        event.id = EventId(event) + (event.ts > 0 ? "|" + std::to_string(event.ts) : "");
        event.index = static_cast<int>(out.size());
        out.push_back(std::move(event));
        continue;
      }

      // BoxOpenLog tem dados autoritativos nos campos IL2CPP. Nao use a regex
      // textual como gate: nomes localizados podem conter simbolos como "1º".
      if (klass_name == "BoxOpenLog") {
        std::wstring full = text + clock_suffix;
        MemoryEvent event;
        event.type = "drop";
        event.color = ExtractColorTag(full);
        event.raw = CleanEventRaw(full);
        event.clock = ExtractClockSuffix(clock_suffix);
        event.ts = stamp;
        event.item = FirstColoredText(text);

        std::string item_name_fallback;
        std::wstring item_key_str = ReadManagedString(process, ReadPtr(process, obj + kBoxOpenItemKeyOffset));
        size_t underscore = item_key_str.rfind(L'_');
        if (underscore != std::wstring::npos) {
          std::string key = Utf8(item_key_str.substr(underscore + 1));
          event.item_key = atoi(key.c_str());
          if (const JsonValue* item = ItemByKey(key)) {
            item_name_fallback = JsonStringValue(ObjectGet(*item, "name"));
            event.category = CategoryFromItemType(JsonStringValue(ObjectGet(*item, "type")));
          }
        }
        if (event.item.empty()) event.item = DropItemNameFromRaw(event.raw);
        if (event.item.empty()) event.item = item_name_fallback;
        if (event.item.empty() && !item_key_str.empty()) event.item = Utf8(item_key_str);

        int grade_value = 10;
        ReadInt32(process, obj + kBoxOpenGradeOffset, grade_value);
        event.grade = GradeTypeName(grade_value);
        event.id = EventId(event) + (event.ts > 0 ? "|" + std::to_string(event.ts) : "");
        event.index = static_cast<int>(out.size());
        out.push_back(std::move(event));
        continue;
      }

      std::vector<MemoryEvent> parsed;
      AppendRegexEvents(parsed, text + clock_suffix);
      if (parsed.empty()) continue;  // tipo de log que nao acompanhamos (level up, etc.)
      MemoryEvent event = std::move(parsed.front());
      event.ts = stamp;

      // Enriquecimento autoritativo a partir da classe IL2CPP do log.
      if (klass_name == "GetBoxLog") {
        event.category = "chest";
      } else if (event.type == "clear" || event.type == "failure") {
        event.category = "stage";
      } else if (event.type == "death") {
        event.category = "death";
      }

      // O relogio [HH:MM] repete a cada dia; o timestamp real torna o id unico.
      event.id = EventId(event) + (event.ts > 0 ? "|" + std::to_string(event.ts) : "");
      event.index = static_cast<int>(out.size());
      out.push_back(std::move(event));
    }
    ok = true;
  } while (false);
  CloseHandle(process);
  return ok;
}

// Reconstroi a ordem cronologica do scan. Cada janela de texto e uma fatia
// contigua (ou copia antiga) do mesmo buffer de log, entao a ordem interna e
// confiavel. As janelas sao reconciliadas por sobreposicao de conteudo: a mais
// longa (o log vivo) vira a base e as demais sao mescladas ancorando os
// eventos em comum. O relogio [HH:MM] nao serve para ordenar, pois se repete
// a cada dia.
std::vector<MemoryEvent> FinalizeScanEvents(std::vector<MemoryEvent>& events) {
  // Agrupa por janela (order_key), com dedupe por id dentro de cada janela.
  std::vector<std::vector<MemoryEvent>> sequences;
  std::vector<std::set<std::string>> seq_seen;
  std::map<int, size_t> seq_by_key;
  for (auto& event : events) {
    if (event.id.empty()) event.id = EventId(event);
    auto it = seq_by_key.find(event.order_key);
    if (it == seq_by_key.end()) {
      it = seq_by_key.emplace(event.order_key, sequences.size()).first;
      sequences.emplace_back();
      seq_seen.emplace_back();
    }
    if (seq_seen[it->second].insert(event.id).second) {
      sequences[it->second].push_back(std::move(event));
    }
  }

  // Da mais longa para a mais curta (estavel na ordem de descoberta).
  std::vector<size_t> order(sequences.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return sequences[a].size() > sequences[b].size(); });

  std::vector<MemoryEvent> master;
  for (size_t si : order) {
    auto& seq = sequences[si];
    if (seq.empty()) continue;
    if (master.empty()) {
      master = std::move(seq);
      continue;
    }
    std::map<std::string, size_t> pos;
    for (size_t i = 0; i < master.size(); ++i) pos.emplace(master[i].id, i);
    std::vector<MemoryEvent> result;
    result.reserve(master.size() + seq.size());
    std::vector<MemoryEvent> pending;  // eventos novos aguardando a proxima ancora
    size_t next = 0;
    for (auto& event : seq) {
      auto found = pos.find(event.id);
      if (found == pos.end()) {
        pending.push_back(std::move(event));
        continue;
      }
      size_t p = found->second;
      if (p >= next) {
        for (size_t i = next; i < p; ++i) result.push_back(std::move(master[i]));
        for (auto& pe : pending) result.push_back(std::move(pe));
        result.push_back(std::move(master[p]));
        next = p + 1;
      } else {
        for (auto& pe : pending) result.push_back(std::move(pe));
      }
      pending.clear();
    }
    // Sem ancora alguma: copia antiga sem sobreposicao, assume anterior ao
    // master. Com ancoras: pendentes ficam logo apos a ultima ancora.
    for (auto& pe : pending) result.push_back(std::move(pe));
    for (size_t i = next; i < master.size(); ++i) result.push_back(std::move(master[i]));
    master = std::move(result);
  }
  return UniqueEvents(master);
}

std::vector<MemoryEvent> ScanMemoryEvents(HANDLE process,
                                          std::vector<MemoryRegion>* cached_regions = nullptr,
                                          bool discover_regions = true,
                                          SIZE_T max_region = 64ULL * 1024ULL * 1024ULL,
                                          size_t context = 4096,
                                          size_t cached_region_limit = 0) {
  std::vector<MemoryEvent> events;
  std::set<std::string> remembered_event_ids;
  struct RegionCandidate {
    MemoryRegion region;
    int clock_key = -1;
    size_t order = 0;
  };
  std::vector<RegionCandidate> region_candidates;
  std::vector<NeedleHit> hits;
  int window_counter = 0;
  auto scan_region = [&](unsigned char* base, SIZE_T size, bool remember) {
    if (!base || size == 0 || size > max_region) return;
    std::vector<unsigned char> data = ReadProcessBytes(process, base, size);
    if (data.empty()) return;
    FindNeedleHits(data.data(), data.size(), hits);
    if (hits.empty()) return;
    // Mescla janelas sobrepostas para decodificar e rodar regex uma unica vez
    // por trecho, em vez de uma vez por ocorrencia.
    size_t i = 0;
    while (i < hits.size()) {
      size_t left = hits[i].index > context ? hits[i].index - context : 0;
      size_t right = (std::min)(data.size(), hits[i].index + context);
      unsigned mask = hits[i].mask;
      size_t j = i + 1;
      while (j < hits.size() && hits[j].index <= right + context) {
        right = (std::max)(right, (std::min)(data.size(), hits[j].index + context));
        mask |= hits[j].mask;
        ++j;
      }
      std::vector<MemoryEvent> found;
      std::wstring text = NormalizeMemoryText(DecodeUtf16Le(data.data() + left, right - left));
      AppendRegexEvents(found, text, mask);
      // Marca a janela de origem: FinalizeScanEvents reconcilia janelas por
      // sobreposicao de conteudo.
      for (auto& event : found) event.order_key = window_counter;
      ++window_counter;
      bool has_new_event = false;
      for (const auto& event : found) {
        std::string id = event.id.empty() ? EventId(event) : event.id;
        if (remembered_event_ids.insert(id).second) has_new_event = true;
      }
      if (remember && cached_regions && has_new_event) {
        int best_clock = -1;
        for (const auto& event : found) best_clock = (std::max)(best_clock, EventClockKey(event));
        region_candidates.push_back(
            {{base + left, static_cast<SIZE_T>(right - left)}, best_clock, region_candidates.size()});
      }
      events.insert(events.end(), found.begin(), found.end());
      i = j;
    }
  };

  if (cached_regions && !cached_regions->empty() && !discover_regions) {
    std::vector<MemoryRegion> still_valid;
    DWORD cached_start = GetTickCount();
    size_t scanned = 0;
    size_t limit = cached_region_limit ? (std::min)(cached_region_limit, cached_regions->size()) : cached_regions->size();
    std::set<std::string> kept_event_ids;
    for (size_t region_index = 0; region_index < limit; ++region_index) {
      const auto& region = (*cached_regions)[region_index];
      size_t before = events.size();
      scan_region(region.base, region.size, false);
      bool has_new_event = false;
      for (size_t i = before; i < events.size(); ++i) {
        std::string id = events[i].id.empty() ? EventId(events[i]) : events[i].id;
        if (kept_event_ids.insert(id).second) has_new_event = true;
      }
      if (has_new_event || cached_region_limit) still_valid.push_back(region);
      ++scanned;
      if (scanned % 50 == 0) {
        PostStatus(L"Cache memoria: " + std::to_wstring(scanned) + L"/" +
                   std::to_wstring(limit) + L" regioes em " +
                   std::to_wstring(GetTickCount() - cached_start) + L"ms.");
      }
    }
    if (!cached_region_limit) {
      *cached_regions = std::move(still_valid);
    }
    return FinalizeScanEvents(events);
  }

  if (cached_regions && discover_regions) cached_regions->clear();
  auto is_private_writable = [](const MEMORY_BASIC_INFORMATION& info) {
    if (info.Type != MEM_PRIVATE) return false;
    DWORD protect = info.Protect & 0xFF;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
  };
  auto walk_regions = [&](bool private_writable_only) {
    unsigned char* address = nullptr;
    MEMORY_BASIC_INFORMATION info{};
    DWORD walk_start = GetTickCount();
    DWORD last_status = walk_start;
    unsigned long long scanned_bytes = 0;
    while (VirtualQueryEx(process, address, &info, sizeof(info)) == sizeof(info)) {
      unsigned char* base = static_cast<unsigned char*>(info.BaseAddress);
      SIZE_T size = info.RegionSize;
      if (IsReadableRegion(info) && size > 0 && size <= max_region &&
          is_private_writable(info) == private_writable_only) {
        scan_region(base, size, true);
        scanned_bytes += size;
        DWORD now = GetTickCount();
        if (now - last_status >= 1000) {
          PostStatus(L"Lendo memoria do jogo: " + std::to_wstring(scanned_bytes / (1024ULL * 1024ULL)) +
                     L" MB lidos, " + std::to_wstring(events.size()) + L" eventos.");
          last_status = now;
        }
      }
      unsigned char* next = base + size;
      if (next <= address) break;
      address = next;
    }
  };
  // Passe rapido: as strings de log do jogo (heap gerenciado) vivem em memoria
  // privada gravavel. So varre o resto do processo se nada for encontrado.
  walk_regions(true);
  if (events.empty()) walk_regions(false);
  if (cached_regions && discover_regions && !region_candidates.empty()) {
    std::sort(region_candidates.begin(), region_candidates.end(), [](const RegionCandidate& a, const RegionCandidate& b) {
      if (a.clock_key != b.clock_key) return a.clock_key > b.clock_key;
      return a.order > b.order;
    });
    std::set<std::string> seen_regions;
    cached_regions->clear();
    for (const auto& candidate : region_candidates) {
      std::string key = std::to_string(reinterpret_cast<unsigned long long>(candidate.region.base)) + ":" +
                        std::to_string(static_cast<unsigned long long>(candidate.region.size));
      if (!seen_regions.insert(key).second) continue;
      cached_regions->push_back(candidate.region);
    }
  }
  return FinalizeScanEvents(events);
}

JsonValue EventJson(const MemoryEvent& event) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "type", JsonValue::String(event.type));
  if (!event.label.empty()) ObjectSet(out, "label", JsonValue::String(event.label));
  if (!event.difficulty.empty()) ObjectSet(out, "difficulty", JsonValue::String(event.difficulty));
  if (event.type == "clear") ObjectSet(out, "seconds", JsonValue::Number(static_cast<long long>(event.seconds)));
  if (!event.progress.empty()) ObjectSet(out, "progress", JsonValue::String(event.progress));
  if (!event.item.empty()) ObjectSet(out, "item", JsonValue::String(event.item));
  if (!event.hero.empty()) ObjectSet(out, "hero", JsonValue::String(event.hero));
  if (!event.enemy.empty()) ObjectSet(out, "enemy", JsonValue::String(event.enemy));
  ObjectSet(out, "clock", JsonValue::String(event.clock));
  if (!event.color.empty()) ObjectSet(out, "color", JsonValue::String(event.color));
  if (!event.category.empty()) ObjectSet(out, "category", JsonValue::String(event.category));
  if (!event.grade.empty()) ObjectSet(out, "grade", JsonValue::String(event.grade));
  if (event.item_key > 0) ObjectSet(out, "itemKey", JsonValue::Number(static_cast<long long>(event.item_key)));
  ObjectSet(out, "raw", JsonValue::String(event.raw));
  ObjectSet(out, "id", JsonValue::String(event.id));
  ObjectSet(out, "index", JsonValue::Number(static_cast<long long>(event.index)));
  if (event.ts > 0) ObjectSet(out, "ts", JsonValue::Number(event.ts));
  return out;
}

std::vector<int> StageParts(const std::string& label) {
  std::vector<int> parts;
  size_t start = 0;
  while (start < label.size()) {
    size_t dash = label.find('-', start);
    std::string part = label.substr(start, dash == std::string::npos ? std::string::npos : dash - start);
    parts.push_back(part.empty() ? 0 : atoi(part.c_str()));
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  return parts;
}

bool StageLabelLess(const std::string& a, const std::string& b) {
  return StageParts(a) < StageParts(b);
}

int DifficultyRank(const std::string& difficulty) {
  if (difficulty == "NORMAL") return 0;
  if (difficulty == "NIGHTMARE") return 1;
  if (difficulty == "HELL") return 2;
  if (difficulty == "TORMENT") return 3;
  return 9;
}

std::string DifficultyByRank(int rank) {
  if (rank <= 0) return "NORMAL";
  if (rank == 1) return "NIGHTMARE";
  if (rank == 2) return "HELL";
  return "TORMENT";
}

std::string DifficultyFromStageKey(long long stage_key) {
  long long tier = stage_key / 1000;
  if (tier == 4) return "TORMENT";
  if (tier == 3) return "HELL";
  if (tier == 2) return "NIGHTMARE";
  return "NORMAL";
}

long long SaveMaxCompletedStage(const SaveSummary& save) {
  if (save.max_completed_stage > 0) return save.max_completed_stage;
  JsonValue root;
  if (ParseJson(save.summary_json, root)) {
    return static_cast<long long>(JsonNumberDouble(ObjectGet(root, "maxCompletedStage")));
  }
  return 0;
}

std::string MaxSaveDifficulty(const SaveSummary& save) {
  std::string current = DifficultyFromStageKey(save.current_stage_key);
  std::string completed = DifficultyFromStageKey(SaveMaxCompletedStage(save));
  return DifficultyRank(completed) > DifficultyRank(current) ? completed : current;
}

bool IsStageEvent(const MemoryEvent& event) {
  return event.type == "clear" || event.type == "failure";
}

std::string EventDifficulty(const MemoryEvent& event, const std::string& fallback) {
  return event.difficulty.empty() ? fallback : event.difficulty;
}

void ApplyDifficultyToStageEvents(std::vector<MemoryEvent>& events, const std::string& difficulty) {
  for (auto& event : events) {
    if (IsStageEvent(event) && event.difficulty.empty()) event.difficulty = difficulty;
  }
}

int StageOrdinal(const std::string& label) {
  std::vector<int> parts = StageParts(label);
  if (parts.size() < 2) return 0;
  return (parts[0] - 1) * 10 + parts[1];
}

int StageOrdinalFromStageKey(long long stage_key) {
  int act = static_cast<int>((stage_key % 1000) / 100);
  int slot = static_cast<int>(stage_key % 100);
  if (act <= 0 || slot <= 0) return 0;
  return (act - 1) * 10 + slot;
}

bool LooksLikeDifficultyWrap(int previous, int current) {
  return previous >= 25 && current > 0 && current <= 5;
}

int StageDifficultyWrapCount(const std::vector<MemoryEvent>& events) {
  int wraps = 0;
  int previous = 0;
  for (const auto& event : events) {
    if (!IsStageEvent(event)) continue;
    int ordinal = StageOrdinal(event.label);
    if (LooksLikeDifficultyWrap(previous, ordinal)) ++wraps;
    if (ordinal > 0) previous = ordinal;
  }
  return wraps;
}

void InferLegacyStageDifficulties(
    std::vector<MemoryEvent>& events,
    const std::string& current_difficulty,
    int max_current_difficulty_ordinal = 30,
    bool overwrite = false) {
  int current_rank = DifficultyRank(current_difficulty);
  if (current_rank == 9) current_rank = 0;
  int wraps = StageDifficultyWrapCount(events);
  int previous = 0;
  int rank = (std::max)(0, current_rank - wraps);
  previous = 0;
  for (auto& event : events) {
    if (!IsStageEvent(event)) continue;
    int ordinal = StageOrdinal(event.label);
    if (LooksLikeDifficultyWrap(previous, ordinal) && rank < current_rank) ++rank;
    int event_rank = rank;
    if (event_rank == current_rank && current_rank > 0 && ordinal > max_current_difficulty_ordinal) {
      event_rank = current_rank - 1;
    }
    if (overwrite || event.difficulty.empty()) event.difficulty = DifficultyByRank(event_rank);
    if (ordinal > 0) previous = ordinal;
  }
}

JsonValue BuildHistoryJson(const std::vector<MemoryEvent>& events, const std::string& difficulty,
                           int events_from_index = -1) {
  struct StageGroup {
    std::string label;
    std::string difficulty;
    std::vector<MemoryEvent> clears;
    std::vector<MemoryEvent> failures;
  };
  struct DropGroup {
    int count = 0;
    std::string last_clock;
    std::string item;
  };
  struct DeathGroup {
    int count = 0;
    std::string last_clock;
    std::string hero;
    std::string enemy;
  };

  std::map<std::string, StageGroup> by_stage;
  std::map<std::string, DropGroup> drops;
  std::map<std::string, DeathGroup> deaths;
  int total_clears = 0;
  int total_failures = 0;
  int total_deaths = 0;
  int total_drops = 0;
  for (const auto& event : events) {
    if (event.type == "clear") {
      ++total_clears;
      std::string event_difficulty = EventDifficulty(event, difficulty);
      std::string key = event_difficulty + "|" + event.label;
      StageGroup& group = by_stage[key];
      group.label = event.label;
      group.difficulty = event_difficulty;
      group.clears.push_back(event);
    } else if (event.type == "failure") {
      ++total_failures;
      std::string event_difficulty = EventDifficulty(event, difficulty);
      std::string key = event_difficulty + "|" + event.label;
      StageGroup& group = by_stage[key];
      group.label = event.label;
      group.difficulty = event_difficulty;
      group.failures.push_back(event);
    } else if (event.type == "death") {
      ++total_deaths;
      std::string key = event.hero + "|" + event.enemy;
      deaths[key].count++;
      deaths[key].last_clock = event.clock;
      deaths[key].hero = event.hero;
      deaths[key].enemy = event.enemy;
    } else if (event.type == "drop") {
      ++total_drops;
      drops[event.item].count++;
      drops[event.item].last_clock = event.clock;
      drops[event.item].item = event.item;
    }
  }

  std::vector<const StageGroup*> stage_groups;
  for (const auto& item : by_stage) stage_groups.push_back(&item.second);
  std::sort(stage_groups.begin(), stage_groups.end(), [](const StageGroup* a, const StageGroup* b) {
    int a_rank = DifficultyRank(a->difficulty);
    int b_rank = DifficultyRank(b->difficulty);
    if (a_rank != b_rank) return a_rank < b_rank;
    return StageLabelLess(a->label, b->label);
  });

  JsonValue stage_summaries = JsonValue::Array();
  for (const StageGroup* group_ptr : stage_groups) {
    const StageGroup& group = *group_ptr;
    std::vector<int> clear_seconds;
    for (const auto& event : group.clears) clear_seconds.push_back(event.seconds);
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "label", JsonValue::String(group.label));
    ObjectSet(row, "difficulty", JsonValue::String(group.difficulty));
    ObjectSet(row, "clears", JsonValue::Number(static_cast<long long>(group.clears.size())));
    ObjectSet(row, "failures", JsonValue::Number(static_cast<long long>(group.failures.size())));
    ObjectSet(row, "attempts", JsonValue::Number(static_cast<long long>(group.clears.size() + group.failures.size())));
    if (!clear_seconds.empty()) {
      int sum = 0;
      int best = clear_seconds[0];
      int worst = clear_seconds[0];
      for (int value : clear_seconds) {
        sum += value;
        best = (std::min)(best, value);
        worst = (std::max)(worst, value);
      }
      size_t start = clear_seconds.size() > 10 ? clear_seconds.size() - 10 : 0;
      int last_sum = 0;
      JsonValue recent = JsonValue::Array();
      for (size_t i = start; i < clear_seconds.size(); ++i) {
        last_sum += clear_seconds[i];
        recent.array.push_back(JsonValue::Number(static_cast<long long>(clear_seconds[i])));
      }
      ObjectSet(row, "average", JsonValue::Number(std::round((static_cast<double>(sum) / clear_seconds.size()) * 100.0) / 100.0));
      ObjectSet(row, "last10Average", JsonValue::Number(std::round((static_cast<double>(last_sum) / (clear_seconds.size() - start)) * 100.0) / 100.0));
      ObjectSet(row, "best", JsonValue::Number(static_cast<long long>(best)));
      ObjectSet(row, "worst", JsonValue::Number(static_cast<long long>(worst)));
      ObjectSet(row, "lastTime", JsonValue::Number(static_cast<long long>(clear_seconds.back())));
      ObjectSet(row, "recentSamples", std::move(recent));
    }
    stage_summaries.array.push_back(std::move(row));
  }

  std::vector<DropGroup> drop_rows;
  for (const auto& item : drops) drop_rows.push_back(item.second);
  std::sort(drop_rows.begin(), drop_rows.end(), [](const DropGroup& a, const DropGroup& b) {
    if (a.count != b.count) return a.count > b.count;
    return LowerAscii(a.item) < LowerAscii(b.item);
  });
  JsonValue drop_json = JsonValue::Array();
  for (const auto& drop : drop_rows) {
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "count", JsonValue::Number(static_cast<long long>(drop.count)));
    ObjectSet(row, "lastClock", JsonValue::String(drop.last_clock));
    ObjectSet(row, "item", JsonValue::String(drop.item));
    drop_json.array.push_back(std::move(row));
  }

  std::vector<DeathGroup> death_rows;
  for (const auto& item : deaths) death_rows.push_back(item.second);
  std::sort(death_rows.begin(), death_rows.end(), [](const DeathGroup& a, const DeathGroup& b) {
    if (a.count != b.count) return a.count > b.count;
    if (LowerAscii(a.hero) != LowerAscii(b.hero)) return LowerAscii(a.hero) < LowerAscii(b.hero);
    return LowerAscii(a.enemy) < LowerAscii(b.enemy);
  });
  JsonValue death_json = JsonValue::Array();
  for (const auto& death : death_rows) {
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "count", JsonValue::Number(static_cast<long long>(death.count)));
    ObjectSet(row, "lastClock", JsonValue::String(death.last_clock));
    ObjectSet(row, "hero", JsonValue::String(death.hero));
    ObjectSet(row, "enemy", JsonValue::String(death.enemy));
    death_json.array.push_back(std::move(row));
  }

  JsonValue event_json = JsonValue::Array();
  for (const auto& event : events) {
    if (events_from_index >= 0 && event.index < events_from_index) continue;
    event_json.array.push_back(EventJson(event));
  }

  JsonValue totals = JsonValue::Object();
  ObjectSet(totals, "clears", JsonValue::Number(static_cast<long long>(total_clears)));
  ObjectSet(totals, "failures", JsonValue::Number(static_cast<long long>(total_failures)));
  ObjectSet(totals, "deaths", JsonValue::Number(static_cast<long long>(total_deaths)));
  ObjectSet(totals, "drops", JsonValue::Number(static_cast<long long>(total_drops)));

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "source", JsonValue::String("memory"));
  ObjectSet(out, "kind", JsonValue::String("log-history"));
  ObjectSet(out, "difficulty", JsonValue::String(difficulty));
  // partial=true: o array "events" contem apenas eventos novos; o servidor
  // mescla por id sem apagar os ja registrados. Resumos/totais sao completos.
  if (events_from_index >= 0) ObjectSet(out, "partial", JsonValue::Bool(true));
  ObjectSet(out, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  ObjectSet(out, "lastEvent", events.empty() ? JsonValue::Null() : EventJson(events.back()));
  ObjectSet(out, "total", JsonValue::Number(static_cast<long long>(events.size())));
  ObjectSet(out, "totals", std::move(totals));
  ObjectSet(out, "stageSummaries", std::move(stage_summaries));
  ObjectSet(out, "drops", std::move(drop_json));
  ObjectSet(out, "deaths", std::move(death_json));
  ObjectSet(out, "events", std::move(event_json));
  return out;
}

JsonValue BuildClearsJson(const std::vector<MemoryEvent>& events, const std::string& difficulty) {
  struct ClearGroup {
    std::string label;
    std::string difficulty;
    std::vector<int> values;
  };
  std::map<std::string, ClearGroup> by_stage;
  const MemoryEvent* last_clear = nullptr;
  for (const auto& event : events) {
    if (event.type != "clear") continue;
    std::string event_difficulty = EventDifficulty(event, difficulty);
    std::string key = event_difficulty + "|" + event.label;
    ClearGroup& group = by_stage[key];
    group.label = event.label;
    group.difficulty = event_difficulty;
    group.values.push_back(event.seconds);
    last_clear = &event;
  }
  std::vector<const ClearGroup*> groups;
  for (const auto& item : by_stage) groups.push_back(&item.second);
  std::sort(groups.begin(), groups.end(), [](const ClearGroup* a, const ClearGroup* b) {
    int a_rank = DifficultyRank(a->difficulty);
    int b_rank = DifficultyRank(b->difficulty);
    if (a_rank != b_rank) return a_rank < b_rank;
    return StageLabelLess(a->label, b->label);
  });

  JsonValue averages = JsonValue::Array();
  for (const ClearGroup* group : groups) {
    const std::vector<int>& values = group->values;
    if (values.empty()) continue;
    size_t start = values.size() > 10 ? values.size() - 10 : 0;
    int sum = 0;
    JsonValue recent = JsonValue::Array();
    for (size_t i = start; i < values.size(); ++i) {
      sum += values[i];
      recent.array.push_back(JsonValue::Number(static_cast<long long>(values[i])));
    }
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "label", JsonValue::String(group->label));
    ObjectSet(row, "difficulty", JsonValue::String(group->difficulty));
    ObjectSet(row, "average", JsonValue::Number(static_cast<double>(sum) / (values.size() - start)));
    ObjectSet(row, "samples", JsonValue::Number(static_cast<long long>(values.size() - start)));
    ObjectSet(row, "lastTime", JsonValue::Number(static_cast<long long>(values.back())));
    ObjectSet(row, "recentSamples", std::move(recent));
    averages.array.push_back(std::move(row));
  }

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "source", JsonValue::String("memory"));
  ObjectSet(out, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  ObjectSet(out, "lastEvent", last_clear ? EventJson(*last_clear) : JsonValue::Null());
  ObjectSet(out, "averages", std::move(averages));
  return out;
}

std::vector<MemoryEvent> MergeMemoryEvents(const std::vector<MemoryEvent>& old_events,
                                           const std::vector<MemoryEvent>& new_events) {
  std::vector<MemoryEvent> merged;
  std::set<std::string> seen;
  auto append = [&](const std::vector<MemoryEvent>& events) {
    for (const auto& event : events) {
      std::string id = event.id.empty() ? EventId(event) : event.id;
      if (!seen.insert(id).second) continue;
      MemoryEvent copy = event;
      copy.id = id;
      copy.index = static_cast<int>(merged.size());
      merged.push_back(std::move(copy));
    }
  };
  append(old_events);
  append(new_events);
  return merged;
}

void RebuildMemorySnapshotJson(MemorySnapshot& snapshot, const std::string& difficulty) {
  snapshot.history_json = JsonSerialize(BuildHistoryJson(snapshot.events, difficulty));
  snapshot.clears_json = JsonSerialize(BuildClearsJson(snapshot.events, difficulty));
}

MemorySnapshot ReadMemorySnapshot(const std::string& difficulty = "NORMAL",
                                  std::vector<MemoryRegion>* cached_regions = nullptr,
                                  bool discover_regions = true,
                                  size_t cached_region_limit = 0) {
  MemorySnapshot snapshot;
  snapshot.pid = FindProcessIdByName(L"TaskBarHero.exe");
  JsonValue watcher = JsonValue::Object();
  ObjectSet(watcher, "source", JsonValue::String("memory"));
  ObjectSet(watcher, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  if (!snapshot.pid) {
    ObjectSet(watcher, "message", JsonValue::String("TaskBarHero.exe not found"));
    snapshot.watcher_json = JsonSerialize(watcher);
    return snapshot;
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, snapshot.pid);
  if (!process) {
    ObjectSet(watcher, "message", JsonValue::String("failed to open TaskBarHero.exe"));
    snapshot.watcher_json = JsonSerialize(watcher);
    return snapshot;
  }
  snapshot.events = ScanMemoryEvents(process, cached_regions, discover_regions, 64ULL * 1024ULL * 1024ULL, 4096,
                                     cached_region_limit);
  ApplyDifficultyToStageEvents(snapshot.events, difficulty);
  CloseHandle(process);
  ObjectSet(watcher, "message", JsonValue::String("memory snapshot pid=" + std::to_string(snapshot.pid) + "; events=" +
                                                  std::to_string(snapshot.events.size()) + "; regions=" +
                                                  std::to_string(cached_regions ? cached_regions->size() : 0)));
  snapshot.watcher_json = JsonSerialize(watcher);
  RebuildMemorySnapshotJson(snapshot, difficulty);
  return snapshot;
}

std::string NarrowAscii(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (wchar_t c : text) out.push_back(c < 128 ? static_cast<char>(c) : '?');
  return out;
}

std::wstring DefaultSavePath() {
  wchar_t profile[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  std::wstring base = length ? std::wstring(profile, length) : L"";
  return base + DEFAULT_SAVE_RELATIVE;
}

bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 16 || size.QuadPart > 64LL * 1024LL * 1024LL) {
    CloseHandle(file);
    return false;
  }
  bytes.resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != bytes.size()) return false;
  return true;
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  BOOL ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
  CloseHandle(file);
  return ok && written == size;
}

bool ReadTextFile(const std::wstring& path, std::string& text) {
  std::vector<unsigned char> bytes;
  if (!ReadAllBytes(path, bytes)) return false;
  text.assign(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return true;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::string utf8 = Utf8(text);
  DWORD written = 0;
  BOOL ok = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  CloseHandle(file);
  return ok && written == utf8.size();
}

std::wstring MemoryRegionsPath() {
  return CompanionDir() + L"\\memory-regions.json";
}

std::string HexAddress(unsigned long long value) {
  char buffer[32]{};
  sprintf_s(buffer, "0x%llx", value);
  return buffer;
}

unsigned long long ParseHexAddress(const std::string& value) {
  const char* text = value.c_str();
  int base = 10;
  if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
    text += 2;
    base = 16;
  }
  return _strtoui64(text, nullptr, base);
}

bool LoadMemoryRegionCache(DWORD pid, std::vector<MemoryRegion>& regions) {
  std::string text;
  if (!ReadTextFile(MemoryRegionsPath(), text)) return false;
  JsonValue root;
  if (!ParseJson(text, root)) return false;
  if (static_cast<DWORD>(JsonNumberDouble(ObjectGet(root, "pid"))) != pid) return false;
  const JsonValue* rows = ObjectGet(root, "regions");
  if (!rows || rows->type != JsonValue::Type::Array) return false;
  std::vector<MemoryRegion> loaded;
  for (const auto& row : rows->array) {
    unsigned long long base = ParseHexAddress(JsonStringValue(ObjectGet(row, "base")));
    unsigned long long size = ParseHexAddress(JsonStringValue(ObjectGet(row, "size")));
    if (base && size) loaded.push_back({reinterpret_cast<unsigned char*>(static_cast<uintptr_t>(base)), static_cast<SIZE_T>(size)});
  }
  if (loaded.empty()) return false;
  regions = std::move(loaded);
  return true;
}

void SaveMemoryRegionCache(DWORD pid, const std::vector<MemoryRegion>& regions) {
  if (!pid || regions.empty()) return;
  JsonValue root = JsonValue::Object();
  ObjectSet(root, "pid", JsonValue::Number(static_cast<long long>(pid)));
  ObjectSet(root, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  JsonValue rows = JsonValue::Array();
  for (const auto& region : regions) {
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "base", JsonValue::String(HexAddress(reinterpret_cast<uintptr_t>(region.base))));
    ObjectSet(row, "size", JsonValue::String(HexAddress(static_cast<unsigned long long>(region.size))));
    rows.array.push_back(std::move(row));
  }
  ObjectSet(root, "regions", std::move(rows));
  WriteTextFile(MemoryRegionsPath(), Widen(JsonSerialize(root)));
}

std::wstring MemoryHistoryPath() {
  return CompanionDir() + L"\\memory-history.json";
}

bool MemoryEventFromJson(const JsonValue& value, MemoryEvent& event) {
  if (value.type != JsonValue::Type::Object) return false;
  event.type = JsonStringValue(ObjectGet(value, "type"));
  if (event.type.empty()) return false;
  event.label = JsonStringValue(ObjectGet(value, "label"));
  event.difficulty = JsonStringValue(ObjectGet(value, "difficulty"));
  event.seconds = static_cast<int>(JsonNumberDouble(ObjectGet(value, "seconds")));
  event.progress = JsonStringValue(ObjectGet(value, "progress"));
  event.item = JsonStringValue(ObjectGet(value, "item"));
  event.hero = JsonStringValue(ObjectGet(value, "hero"));
  event.enemy = JsonStringValue(ObjectGet(value, "enemy"));
  event.clock = JsonStringValue(ObjectGet(value, "clock"));
  event.color = JsonStringValue(ObjectGet(value, "color"));
  event.category = JsonStringValue(ObjectGet(value, "category"));
  event.grade = JsonStringValue(ObjectGet(value, "grade"));
  event.item_key = static_cast<int>(JsonNumberDouble(ObjectGet(value, "itemKey")));
  event.raw = JsonStringValue(ObjectGet(value, "raw"));
  event.id = JsonStringValue(ObjectGet(value, "id"));
  if (event.id.empty()) event.id = EventId(event);
  event.index = static_cast<int>(JsonNumberDouble(ObjectGet(value, "index")));
  event.ts = static_cast<long long>(JsonNumberDouble(ObjectGet(value, "ts")));
  return true;
}

bool LoadMemoryHistoryCache(MemorySnapshot& snapshot, const std::string& difficulty, long long max_completed_stage_key = 0) {
  std::string text;
  if (!ReadTextFile(MemoryHistoryPath(), text)) return false;
  JsonValue root;
  if (!ParseJson(text, root)) return false;
  std::string cached_difficulty = JsonStringValue(ObjectGet(root, "difficulty"));
  if (cached_difficulty.empty()) cached_difficulty = difficulty;
  const JsonValue* events = ObjectGet(root, "events");
  if (!events || events->type != JsonValue::Type::Array) return false;
  std::vector<MemoryEvent> loaded;
  bool has_stage_difficulty = false;
  bool missing_stage_difficulty = false;
  bool mixed_stage_difficulty = false;
  bool has_impossible_max_difficulty_stage = false;
  int uniform_stage_rank = -1;
  int max_difficulty_rank = DifficultyRank(difficulty);
  int max_difficulty_ordinal = StageOrdinalFromStageKey(max_completed_stage_key);
  if (max_difficulty_ordinal <= 0) max_difficulty_ordinal = 30;
  for (const auto& item : events->array) {
    MemoryEvent event;
    if (MemoryEventFromJson(item, event)) {
      if (IsStageEvent(event)) {
        if (event.difficulty.empty()) missing_stage_difficulty = true;
        else {
          has_stage_difficulty = true;
          int rank = DifficultyRank(event.difficulty);
          if (uniform_stage_rank < 0) uniform_stage_rank = rank;
          else if (rank != uniform_stage_rank) mixed_stage_difficulty = true;
          if (rank == max_difficulty_rank && max_difficulty_rank > 0 && StageOrdinal(event.label) > max_difficulty_ordinal) {
            has_impossible_max_difficulty_stage = true;
          }
        }
      }
      loaded.push_back(std::move(event));
    }
  }
  if (loaded.empty()) return false;
  if (missing_stage_difficulty && !has_stage_difficulty) {
    InferLegacyStageDifficulties(loaded, difficulty, max_difficulty_ordinal);
  } else if (
      has_stage_difficulty &&
      !missing_stage_difficulty &&
      !mixed_stage_difficulty &&
      uniform_stage_rank >= 0 &&
      uniform_stage_rank < DifficultyRank(difficulty) &&
      StageDifficultyWrapCount(loaded) > 0) {
    InferLegacyStageDifficulties(loaded, difficulty, max_difficulty_ordinal, true);
  } else if (has_impossible_max_difficulty_stage && StageDifficultyWrapCount(loaded) > 0) {
    InferLegacyStageDifficulties(loaded, difficulty, max_difficulty_ordinal, true);
  }
  ApplyDifficultyToStageEvents(loaded, cached_difficulty);
  snapshot.events = UniqueEvents(loaded);
  RebuildMemorySnapshotJson(snapshot, difficulty);
  return true;
}

void SaveMemoryHistoryCache(const MemorySnapshot& snapshot) {
  if (snapshot.history_json.empty() || snapshot.history_json == "null") return;
  WriteTextFile(MemoryHistoryPath(), Widen(snapshot.history_json));
}

std::wstring SyncStatePath() {
  return CompanionDir() + L"\\sync-state.json";
}

void LoadSyncState(WorkerState& state) {
  std::string text;
  if (!ReadTextFile(SyncStatePath(), text)) return;
  JsonValue root;
  if (!ParseJson(text, root)) return;
  state.synced_index = static_cast<long long>(JsonNumberDouble(ObjectGet(root, "lastIndex"), -1));
  state.synced_first_id = JsonStringValue(ObjectGet(root, "firstId"));
}

void SaveSyncState(const WorkerState& state) {
  JsonValue root = JsonValue::Object();
  ObjectSet(root, "lastIndex", JsonValue::Number(state.synced_index));
  ObjectSet(root, "firstId", JsonValue::String(state.synced_first_id));
  WriteTextFile(SyncStatePath(), Widen(JsonSerialize(root)));
}

std::wstring TempComparePath() {
  wchar_t temp[MAX_PATH]{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  std::wstring dir = length ? std::wstring(temp, length) : L".\\";
  return dir + L"tbh-agent-compare.txt";
}

std::wstring TempSaveSummaryPath() {
  wchar_t temp[MAX_PATH]{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  std::wstring dir = length ? std::wstring(temp, length) : L".\\";
  return dir + L"tbh-agent-save-summary.json";
}

std::wstring ParentDir(std::wstring path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

bool DirectoryExists(const std::wstring& path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectoryTree(const std::wstring& dir) {
  if (dir.empty()) return false;
  if (DirectoryExists(dir)) return true;

  std::wstring parent = ParentDir(dir);
  if (!parent.empty() && parent != dir && !DirectoryExists(parent)) {
    if (!EnsureDirectoryTree(parent)) return false;
  }

  if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS && DirectoryExists(dir);
}

std::wstring ExePath() {
  std::vector<wchar_t> path(MAX_PATH);
  DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  while (length == path.size()) {
    path.resize(path.size() * 2);
    length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  }
  return std::wstring(path.data(), length);
}

bool WriteUtf8TextFileAtomic(const std::wstring& path, const std::string& text) {
  if (path.empty()) return false;
  std::wstring temp = path + L".tmp";
  if (!WriteAllBytes(temp, text.data(), text.size())) return false;
  if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  DeleteFileW(temp.c_str());
  return false;
}

std::wstring ItemsJsonPath() {
  static std::wstring resolved;
  if (!resolved.empty()) return resolved;

  std::wstring items_root = CompanionDir() + L"\\items";
  CreateDirectoryW(items_root.c_str(), nullptr);
  std::wstring cache_dir = items_root + L"\\" + EMBEDDED_ITEMS_SHA1;
  CreateDirectoryW(cache_dir.c_str(), nullptr);
  std::wstring cached_json = cache_dir + L"\\items.json";
  DWORD attrs = GetFileAttributesW(cached_json.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    resolved = cached_json;
    return resolved;
  }

  HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_ITEMS_ZIP), RT_RCDATA);
  HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
  void* data = loaded ? LockResource(loaded) : nullptr;
  DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
  std::wstring zip_path = cache_dir + L"\\items.zip";
  if (data && size && WriteAllBytes(zip_path, data, size)) {
    auto quote_ps = [](std::wstring value) {
      std::wstring out = L"'";
      for (wchar_t c : value) {
        if (c == L'\'') out += L"''";
        else out.push_back(c);
      }
      out += L"'";
      return out;
    };
    std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath " +
                           quote_ps(zip_path) + L" -DestinationPath " + quote_ps(cache_dir) + L" -Force\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
      WaitForSingleObject(process.hProcess, 60000);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
    attrs = GetFileAttributesW(cached_json.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      resolved = cached_json;
      return resolved;
    }
  }

  resolved = cached_json;
  return resolved;
}

bool Es3AesKey(const std::string& password, const std::vector<unsigned char>& salt, std::vector<unsigned char>& key) {
  key.assign(16, 0);
  BCRYPT_ALG_HANDLE alg = nullptr;
  NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (status >= 0) {
    status = BCryptDeriveKeyPBKDF2(
        alg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()),
        static_cast<ULONG>(salt.size()),
        100,
        key.data(),
        static_cast<ULONG>(key.size()),
        0);
  }
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  return status >= 0;
}

bool DecryptEs3AesCbc(const std::vector<unsigned char>& file_bytes, const std::string& password, std::string& plaintext) {
  if (file_bytes.size() <= 16) return false;
  std::vector<unsigned char> iv(file_bytes.begin(), file_bytes.begin() + 16);
  std::vector<unsigned char> cipher(file_bytes.begin() + 16, file_bytes.end());
  std::vector<unsigned char> key;
  if (!Es3AesKey(password, iv, key)) return false;

  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE key_handle = nullptr;
  DWORD object_length = 0;
  DWORD data_length = 0;
  NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
  if (status < 0) return false;
  status = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                             static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_CBC) + 1) * sizeof(wchar_t)), 0);
  if (status >= 0) {
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &data_length, 0);
  }
  std::vector<unsigned char> key_object(object_length);
  if (status >= 0) {
    status = BCryptGenerateSymmetricKey(alg, &key_handle, key_object.data(), object_length, key.data(), static_cast<ULONG>(key.size()), 0);
  }

  DWORD plain_size = 0;
  std::vector<unsigned char> iv_copy = iv;
  if (status >= 0) {
    status = BCryptDecrypt(key_handle, cipher.data(), static_cast<ULONG>(cipher.size()), nullptr, iv_copy.data(), static_cast<ULONG>(iv_copy.size()),
                           nullptr, 0, &plain_size, BCRYPT_BLOCK_PADDING);
  }
  std::vector<unsigned char> plain(plain_size);
  iv_copy = iv;
  if (status >= 0) {
    status = BCryptDecrypt(key_handle, cipher.data(), static_cast<ULONG>(cipher.size()), nullptr, iv_copy.data(), static_cast<ULONG>(iv_copy.size()),
                           plain.data(), plain_size, &plain_size, BCRYPT_BLOCK_PADDING);
  }
  if (key_handle) BCryptDestroyKey(key_handle);
  if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  if (status < 0) return false;
  plaintext.assign(reinterpret_cast<char*>(plain.data()), plain_size);
  return true;
}

std::string ExtractJsonString(const std::string& json, const std::string& key, size_t start = 0) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, start);
  if (pos == std::string::npos) pos = json.find(key, start);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos + key.size());
  if (pos == std::string::npos) return {};
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return {};
  bool escaped_json_string = pos > 0 && json[pos - 1] == '\\';
  std::string value;
  bool escape = false;
  for (size_t i = pos + 1; i < json.size(); ++i) {
    char c = json[i];
    if (escaped_json_string && c == '\\' && i + 1 < json.size() && json[i + 1] == '"') {
      return value;
    }
    if (escape) {
      value.push_back(c);
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      return value;
    } else {
      value.push_back(c);
    }
  }
  return {};
}

long long ExtractJsonInt(const std::string& json, const std::string& key, size_t start = 0, long long fallback = 0) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle, start);
  if (pos == std::string::npos) pos = json.find(key, start);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos + key.size());
  if (pos == std::string::npos) return fallback;
  ++pos;
  while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  bool negative = false;
  if (pos < json.size() && json[pos] == '-') {
    negative = true;
    ++pos;
  }
  long long value = 0;
  bool any = false;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    any = true;
    value = value * 10 + (json[pos] - '0');
    ++pos;
  }
  return any ? (negative ? -value : value) : fallback;
}

long long ExtractLastJsonInt(const std::string& json, const std::string& key, long long fallback = 0) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.rfind(needle);
  if (pos == std::string::npos) return fallback;
  return ExtractJsonInt(json, key, pos, fallback);
}

std::string ExtractSection(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) pos = json.find(key);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos + key.size());
  if (pos == std::string::npos) return {};
  ++pos;
  while (pos < json.size() && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size()) return {};
  char open = json[pos];
  char close = open == '{' ? '}' : open == '[' ? ']' : '\0';
  if (!close) return {};
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = pos; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) continue;
    if (c == open) ++depth;
    if (c == close) {
      --depth;
      if (depth == 0) return json.substr(pos, i - pos + 1);
    }
  }
  return {};
}

std::string ExtractArraySection(const std::string& json, const std::string& key) {
  std::string section = ExtractSection(json, key);
  return section.empty() ? "[]" : section;
}

size_t FindKey(const std::string& text, const std::string& key, size_t start) {
  size_t quoted = text.find("\"" + key + "\"", start);
  size_t escaped = text.find("\\\"" + key + "\\\"", start);
  size_t plain = text.find(key, start);
  size_t best = std::string::npos;
  for (size_t pos : {quoted, escaped, plain}) {
    if (pos != std::string::npos && (best == std::string::npos || pos < best)) best = pos;
  }
  return best;
}

int CountKey(const std::string& text, const std::string& key) {
  int count = 0;
  size_t pos = 0;
  while ((pos = FindKey(text, key, pos)) != std::string::npos) {
    ++count;
    pos += key.size();
  }
  return count;
}

std::string BuildPetsSummaryJson(const std::string& pets_section) {
  std::string output = "[";
  size_t pos = 0;
  bool first = true;
  while ((pos = FindKey(pets_section, "PetKey", pos)) != std::string::npos) {
    long long key = ExtractJsonInt(pets_section, "PetKey", pos);
    long long unlocked = ExtractJsonString(pets_section, "IsUnlock", pos) == "true" ? 1 : ExtractJsonInt(pets_section, "IsUnlock", pos, -1);
    long long viewed = ExtractJsonString(pets_section, "IsViewed", pos) == "true" ? 1 : ExtractJsonInt(pets_section, "IsViewed", pos, -1);

    size_t object_end = pets_section.find('}', pos);
    std::string object = object_end == std::string::npos ? pets_section.substr(pos) : pets_section.substr(pos, object_end - pos);
    bool is_unlocked = object.find("\"IsUnlock\":true") != std::string::npos || object.find("\\\"IsUnlock\\\":true") != std::string::npos || unlocked == 1;
    bool is_viewed = object.find("\"IsViewed\":true") != std::string::npos || object.find("\\\"IsViewed\\\":true") != std::string::npos || viewed == 1;

    if (!first) output += ",";
    first = false;
    output += "{\"petKey\":" + std::to_string(key) + ",\"unlocked\":" + (is_unlocked ? "true" : "false") +
              ",\"viewed\":" + (is_viewed ? "true" : "false") + "}";
    pos += 8;
  }
  output += "]";
  return output;
}

std::string BuildRunesSummaryJson(const std::string& runes_section) {
  std::string output = "[";
  size_t pos = 0;
  bool first = true;
  while ((pos = FindKey(runes_section, "RuneKey", pos)) != std::string::npos) {
    long long key = ExtractJsonInt(runes_section, "RuneKey", pos);
    long long level = ExtractJsonInt(runes_section, "Level", pos);
    if (!first) output += ",";
    first = false;
    output += "{\"runeKey\":" + std::to_string(key) + ",\"level\":" + std::to_string(level) + "}";
    pos += 9;
  }
  output += "]";
  return output;
}

const JsonValue* FindByNumberKey(const JsonValue* array, const std::string& field, const std::string& wanted) {
  if (!array || array->type != JsonValue::Type::Array) return nullptr;
  for (const auto& item : array->array) {
    if (JsonNumberKey(ObjectGet(item, field)) == wanted) return &item;
  }
  return nullptr;
}

std::map<std::string, const JsonValue*> IndexArrayByNumberKey(const JsonValue* array, const std::string& field) {
  std::map<std::string, const JsonValue*> index;
  if (!array || array->type != JsonValue::Type::Array) return index;
  for (const auto& item : array->array) {
    std::string key = JsonNumberKey(ObjectGet(item, field));
    if (!key.empty()) index[key] = &item;
  }
  return index;
}

JsonValue CopyOrNull(const JsonValue* value) {
  return value ? *value : JsonValue::Null();
}

JsonValue CopyOrEmptyArray(const JsonValue* value) {
  if (value && value->type == JsonValue::Type::Array) return *value;
  return JsonValue::Array();
}

JsonValue BuildStatSummary(const JsonValue& stat, const std::string& section) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "stat", CopyOrNull(ObjectGet(stat, "stat")));
  ObjectSet(out, "mod", CopyOrNull(ObjectGet(stat, "mod")));
  ObjectSet(out, "value", CopyOrNull(ObjectGet(stat, "value")));
  ObjectSet(out, "display", CopyOrNull(ObjectGet(stat, "disp")));
  ObjectSet(out, "section", JsonValue::String(section));
  return out;
}

std::string BonusType(const JsonValue& stat) {
  std::string name = JsonStringValue(ObjectGet(stat, "stat"));
  if (name == "IncreaseExpAmount") return "exp";
  if (name == "IncreaseGoldAmount") return "gold";
  return {};
}

JsonValue StatDisplayValue(const JsonValue& stat) {
  const JsonValue* display = ObjectGet(stat, "disp");
  if (!display) display = ObjectGet(stat, "display");
  return CopyOrNull(display);
}

JsonValue BuildBonusStatSummary(const JsonValue& stat, const std::string& section, const std::string& type, double percent) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "type", JsonValue::String(type));
  ObjectSet(out, "stat", CopyOrNull(ObjectGet(stat, "stat")));
  ObjectSet(out, "percent", JsonValue::Number(percent));
  ObjectSet(out, "display", StatDisplayValue(stat));
  ObjectSet(out, "section", JsonValue::String(section));
  return out;
}

JsonValue BuildBonusSource(const JsonValue& hero, const JsonValue* item, const JsonValue* item_key, const std::string& type, double percent, const JsonValue& stat) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "heroKey", CopyOrNull(ObjectGet(hero, "heroKey")));
  ObjectSet(out, "itemKey", CopyOrNull(item_key));
  ObjectSet(out, "name", item ? CopyOrNull(ObjectGet(*item, "name")) : JsonValue::Null());
  ObjectSet(out, "type", JsonValue::String(type));
  ObjectSet(out, "percent", JsonValue::Number(percent));
  ObjectSet(out, "display", StatDisplayValue(stat));
  return out;
}

struct StatModInfo {
  long long key;
  const char* stat;
  const char* mod;
};

const StatModInfo* FindStatModInfo(long long key) {
  static const StatModInfo kStatMods[] = {
      {100101, "AttackDamage", "FLAT"},
      {100102, "AttackDamage", "ADDITIVE"},
      {100201, "AttackSpeed", "ADDITIVE"},
      {100301, "CriticalChance", "FLAT"},
      {100302, "CriticalChance", "ADDITIVE"},
      {100401, "CriticalDamage", "FLAT"},
      {100501, "MaxHp", "FLAT"},
      {100502, "MaxHp", "ADDITIVE"},
      {100601, "Armor", "FLAT"},
      {100602, "Armor", "ADDITIVE"},
      {100701, "CooldownReduction", "FLAT"},
      {100801, "MovementSpeed", "ADDITIVE"},
      {100901, "AreaOfEffect", "ADDITIVE"},
      {101001, "BaseAttackCountReduction", "FLAT"},
      {101101, "SkillRangeExpansion", "FLAT"},
      {101201, "FireResistance", "FLAT"},
      {101301, "ColdResistance", "FLAT"},
      {101401, "LightningResistance", "FLAT"},
      {101501, "ChaosResistance", "FLAT"},
      {101601, "DodgeChance", "FLAT"},
      {101701, "BlockChance", "FLAT"},
      {101801, "MaxDodgeChance", "FLAT"},
      {101901, "MaxBlockChance", "FLAT"},
      {102001, "Multistrike", "FLAT"},
      {102101, "HpLeech", "FLAT"},
      {102201, "ProjectileCount", "FLAT"},
      {102301, "HpRegenPerSec", "FLAT"},
      {102401, "PhysicalDamagePercent", "FLAT"},
      {102501, "FireDamagePercent", "FLAT"},
      {102601, "ColdDamagePercent", "FLAT"},
      {102701, "LightningDamagePercent", "FLAT"},
      {102801, "ChaosDamagePercent", "FLAT"},
      {102901, "MaxFireResistance", "FLAT"},
      {103001, "MaxColdResistance", "FLAT"},
      {103101, "MaxLightningResistance", "FLAT"},
      {103201, "MaxChaosResistance", "FLAT"},
      {103301, "AddHpPerHit", "FLAT"},
      {103401, "DamageReduction", "FLAT"},
      {103501, "PhysicalDamageReduction", "FLAT"},
      {103601, "FireDamageReduction", "FLAT"},
      {103701, "ColdDamageReduction", "FLAT"},
      {103801, "LightningDamageReduction", "FLAT"},
      {103901, "ChaosDamageReduction", "FLAT"},
      {104001, "DamageAbsorption", "FLAT"},
      {104101, "DamageAddition", "FLAT"},
      {104201, "PhysicalDamageAddition", "FLAT"},
      {104301, "FireDamageAddition", "FLAT"},
      {104401, "ColdDamageAddition", "FLAT"},
      {104501, "LightningDamageAddition", "FLAT"},
      {104601, "ChaosDamageAddition", "FLAT"},
      {104701, "IncreaseExpAmount", "FLAT"},
      {104801, "AdditionalExp", "FLAT"},
      {104901, "CastSpeed", "ADDITIVE"},
      {105201, "AllElementalResistance", "FLAT"},
      {105202, "MovementSpeed", "FLAT"},
      {105301, "AddHpPerKill", "FLAT"},
      {105401, "IncreaseAreaOfEffectDamage", "ADDITIVE"},
      {105501, "IncreaseMeleeDamage", "ADDITIVE"},
      {105601, "IncreaseProjectileDamage", "ADDITIVE"},
      {105701, "IncreaseSummonDamage", "ADDITIVE"},
      {105801, "SkillDurationIncrease", "ADDITIVE"},
      {105901, "SkillHealIncrease", "ADDITIVE"},
  };
  for (const auto& info : kStatMods) {
    if (info.key == key) return &info;
  }
  return nullptr;
}

const char* StatTypeName(long long type) {
  switch (type) {
    case 1: return "AttackDamage";
    case 2: return "AttackSpeed";
    case 3: return "CriticalChance";
    case 4: return "CriticalDamage";
    case 5: return "MaxHp";
    case 6: return "Armor";
    case 7: return "CooldownReduction";
    case 8: return "MovementSpeed";
    case 9: return "AreaOfEffect";
    case 10: return "BaseAttackCountReduction";
    case 11: return "SkillRangeExpansion";
    case 12: return "FireResistance";
    case 13: return "ColdResistance";
    case 14: return "LightningResistance";
    case 15: return "ChaosResistance";
    case 16: return "DodgeChance";
    case 17: return "BlockChance";
    case 18: return "MaxDodgeChance";
    case 19: return "MaxBlockChance";
    case 20: return "Multistrike";
    case 21: return "HpLeech";
    case 22: return "ProjectileCount";
    case 23: return "HpRegenPerSec";
    case 24: return "PhysicalDamagePercent";
    case 25: return "FireDamagePercent";
    case 26: return "ColdDamagePercent";
    case 27: return "LightningDamagePercent";
    case 28: return "ChaosDamagePercent";
    case 29: return "MaxFireResistance";
    case 30: return "MaxColdResistance";
    case 31: return "MaxLightningResistance";
    case 32: return "MaxChaosResistance";
    case 33: return "AddHpPerHit";
    case 34: return "DamageReduction";
    case 35: return "PhysicalDamageReduction";
    case 36: return "FireDamageReduction";
    case 37: return "ColdDamageReduction";
    case 38: return "LightningDamageReduction";
    case 39: return "ChaosDamageReduction";
    case 40: return "DamageAbsorption";
    case 41: return "DamageAddition";
    case 42: return "PhysicalDamageAddition";
    case 43: return "FireDamageAddition";
    case 44: return "ColdDamageAddition";
    case 45: return "LightningDamageAddition";
    case 46: return "ChaosDamageAddition";
    case 47: return "IncreaseExpAmount";
    case 48: return "AdditionalExp";
    case 49: return "CastSpeed";
    case 52: return "AllElementalResistance";
    case 53: return "AddHpPerKill";
    case 54: return "IncreaseAreaOfEffectDamage";
    case 55: return "IncreaseMeleeDamage";
    case 56: return "IncreaseProjectileDamage";
    case 57: return "IncreaseSummonDamage";
    case 58: return "SkillDurationIncrease";
    case 59: return "SkillHealIncrease";
    default: return "";
  }
}

const char* ModTypeName(long long type) {
  switch (type) {
    case 1: return "ADDITIVE";
    case 2: return "MULTIPLICATIVE";
    default: return "FLAT";
  }
}

std::string TrimTrailingZeros(std::string value) {
  size_t dot = value.find('.');
  if (dot == std::string::npos) return value;
  while (!value.empty() && value.back() == '0') value.pop_back();
  if (!value.empty() && value.back() == '.') value.pop_back();
  return value.empty() ? "0" : value;
}

std::string FormatScaledValue(double value, double divisor, const std::string& suffix = "") {
  double scaled = divisor == 0 ? value : value / divisor;
  std::ostringstream stream;
  if (std::fabs(scaled - std::round(scaled)) < 0.000000001) {
    stream << static_cast<long long>(std::llround(scaled));
  } else {
    stream.precision(3);
    stream << std::fixed << scaled;
  }
  return TrimTrailingZeros(stream.str()) + suffix;
}

bool IsDirectPercentStat(const std::string& stat) {
  return stat == "FireResistance" || stat == "ColdResistance" || stat == "LightningResistance" ||
         stat == "ChaosResistance" || stat == "AllElementalResistance" ||
         stat == "MaxFireResistance" || stat == "MaxColdResistance" ||
         stat == "MaxLightningResistance" || stat == "MaxChaosResistance";
}

bool IsTenthsPercentStat(const std::string& stat, const std::string& mod) {
  if (mod == "ADDITIVE" || mod == "MULTIPLICATIVE") return true;
  return stat == "AttackSpeed" || stat == "CriticalChance" || stat == "CriticalDamage" ||
         stat == "CooldownReduction" || stat == "DodgeChance" || stat == "BlockChance" ||
         stat == "MaxDodgeChance" || stat == "MaxBlockChance" || stat == "HpLeech" ||
         stat == "DamageReduction" || stat == "PhysicalDamageReduction" ||
         stat == "FireDamageReduction" || stat == "ColdDamageReduction" ||
         stat == "LightningDamageReduction" || stat == "ChaosDamageReduction" ||
         stat == "PhysicalDamagePercent" || stat == "FireDamagePercent" ||
         stat == "ColdDamagePercent" || stat == "LightningDamagePercent" ||
         stat == "ChaosDamagePercent" || stat == "DamageAddition" ||
         stat == "PhysicalDamageAddition" || stat == "FireDamageAddition" ||
         stat == "ColdDamageAddition" || stat == "LightningDamageAddition" ||
         stat == "ChaosDamageAddition" || stat == "IncreaseExpAmount";
}

std::string FormatEnchantDisplay(const std::string& stat, const std::string& mod, double value) {
  std::string body;
  if (stat == "HpRegenPerSec") {
    body = FormatScaledValue(value, 100.0);
  } else if (stat == "DamageAbsorption") {
    body = FormatScaledValue(value, 10.0);
  } else if (IsDirectPercentStat(stat)) {
    body = FormatScaledValue(value, 1.0, "%");
  } else if (IsTenthsPercentStat(stat, mod)) {
    body = FormatScaledValue(value, 10.0, "%");
  } else {
    body = FormatScaledValue(value, 1.0);
  }
  return (value > 0 ? "+" : "") + body;
}

const char* EnchantSectionName(long long recipe_type) {
  switch (recipe_type) {
    case 3: return "decoration";
    case 4: return "engraving";
    case 5: return "inscription";
    default: return "";
  }
}

long long SlotCapacity(const JsonValue* item, const std::string& section) {
  if (!item) return 0;
  const JsonValue* slots = ObjectGet(*item, "slots");
  return static_cast<long long>(JsonNumberDouble(slots ? ObjectGet(*slots, section) : nullptr));
}

long long EnchantCountByIndex(const JsonValue& saved_item, size_t index) {
  const JsonValue* counts = ObjectGet(saved_item, "EnchantCount");
  if (!counts || counts->type != JsonValue::Type::Array || counts->array.size() <= index) return 0;
  return static_cast<long long>(JsonNumberDouble(&counts->array[index]));
}

long long EnchantAppliedTotal(const JsonValue& saved_item, const std::string& section) {
  if (section == "decoration") return static_cast<long long>(JsonNumberDouble(ObjectGet(saved_item, "DecorationAppliedTotalCount")));
  if (section == "engraving") return static_cast<long long>(JsonNumberDouble(ObjectGet(saved_item, "EngravingAppliedTotalCount")));
  if (section == "inscription") return static_cast<long long>(JsonNumberDouble(ObjectGet(saved_item, "InscriptionAppliedTotalCount")));
  return 0;
}

JsonValue BuildEnchantStatSummary(const JsonValue& enchant, const std::string& section, long long slot) {
  long long stat_mod_key = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "StatModKey")));
  long long stat_type = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "StatType")));
  long long mod_type = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "ModType")));
  long long tier = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "Tier")));
  double value = JsonNumberDouble(ObjectGet(enchant, "Value"));

  const StatModInfo* info = FindStatModInfo(stat_mod_key);
  std::string stat = info ? info->stat : StatTypeName(stat_type);
  std::string mod = info ? info->mod : ModTypeName(mod_type);
  if (stat.empty()) stat = "Stat" + std::to_string(stat_type);

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "stat", JsonValue::String(stat));
  ObjectSet(out, "mod", JsonValue::String(mod));
  ObjectSet(out, "value", JsonValue::Number(value));
  ObjectSet(out, "display", JsonValue::String(FormatEnchantDisplay(stat, mod, value)));
  ObjectSet(out, "section", JsonValue::String(section));
  ObjectSet(out, "slotType", JsonValue::String(section));
  ObjectSet(out, "slot", JsonValue::Number(slot));
  ObjectSet(out, "tier", JsonValue::Number(tier));
  ObjectSet(out, "statModKey", JsonValue::Number(stat_mod_key));
  ObjectSet(out, "statType", JsonValue::Number(stat_type));
  ObjectSet(out, "recipeType", CopyOrNull(ObjectGet(enchant, "RecipeType")));
  ObjectSet(out, "materialKey", CopyOrNull(ObjectGet(enchant, "MaterialKey")));
  return out;
}

JsonValue BuildEnchantSlots(const JsonValue& saved_item, const JsonValue* item, JsonValue& all_stats,
                            JsonValue& bonus_stats, const JsonValue& hero, const JsonValue* item_key,
                            double& exp_bonus, double& gold_bonus, JsonValue& bonus_sources) {
  JsonValue slots = JsonValue::Object();
  std::map<std::string, long long> filled_counts;
  std::map<std::string, JsonValue> items_by_section;
  for (const std::string section : {"decoration", "engraving", "inscription"}) {
    items_by_section[section] = JsonValue::Array();
    filled_counts[section] = 0;
  }

  const JsonValue* enchants = ObjectGet(saved_item, "EnchantData");
  if (enchants && enchants->type == JsonValue::Type::Array) {
    for (const auto& enchant : enchants->array) {
      long long recipe_type = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "RecipeType")));
      long long stat_mod_key = static_cast<long long>(JsonNumberDouble(ObjectGet(enchant, "StatModKey")));
      double value = JsonNumberDouble(ObjectGet(enchant, "Value"));
      const char* section_name = EnchantSectionName(recipe_type);
      if (!section_name[0] || (stat_mod_key == 0 && std::fabs(value) < 0.000000001)) continue;
      std::string section = section_name;
      long long slot = ++filled_counts[section];

      JsonValue stat = BuildEnchantStatSummary(enchant, section, slot);
      all_stats.array.push_back(stat);

      std::string type = BonusType(stat);
      double percent = JsonNumberDouble(ObjectGet(stat, "value")) / 10.0;
      if (!type.empty() && std::fabs(percent) > 0.000000001) {
        bonus_stats.array.push_back(BuildBonusStatSummary(stat, section, type, percent));
        if (type == "exp") exp_bonus += percent;
        if (type == "gold") gold_bonus += percent;
        bonus_sources.array.push_back(BuildBonusSource(hero, item, item_key, type, percent, stat));
      }
      items_by_section[section].array.push_back(std::move(stat));
    }
  }

  size_t count_index = 0;
  for (const std::string section : {"decoration", "engraving", "inscription"}) {
    long long count = (std::max)(filled_counts[section], EnchantCountByIndex(saved_item, count_index++));
    count = (std::max)(count, EnchantAppliedTotal(saved_item, section));
    JsonValue section_value = JsonValue::Object();
    ObjectSet(section_value, "capacity", JsonValue::Number(SlotCapacity(item, section)));
    ObjectSet(section_value, "filled", JsonValue::Number(count));
    ObjectSet(section_value, "items", std::move(items_by_section[section]));
    ObjectSet(slots, section, std::move(section_value));
  }
  return slots;
}

JsonValue BuildSavedItemSummary(const JsonValue& saved_item,
                                const JsonValue* item,
                                const JsonValue* item_key,
                                const JsonValue& bonus_owner,
                                double& exp_bonus,
                                double& gold_bonus,
                                JsonValue& bonus_sources) {
  JsonValue all_stats = JsonValue::Array();
  JsonValue bonus_stats = JsonValue::Array();
  const JsonValue* stats = item ? ObjectGet(*item, "stats") : nullptr;
  for (const std::string section_name : {"base", "inherent"}) {
    const JsonValue* section = stats ? ObjectGet(*stats, section_name) : nullptr;
    if (!section || section->type != JsonValue::Type::Array) continue;
    for (const auto& stat : section->array) {
      all_stats.array.push_back(BuildStatSummary(stat, section_name));
      std::string type = BonusType(stat);
      double value = JsonNumberDouble(ObjectGet(stat, "value"));
      double percent = value / 10.0;
      if (!type.empty() && std::fabs(percent) > 0.000000001) {
        bonus_stats.array.push_back(BuildBonusStatSummary(stat, section_name, type, percent));
        if (type == "exp") exp_bonus += percent;
        if (type == "gold") gold_bonus += percent;
        bonus_sources.array.push_back(BuildBonusSource(bonus_owner, item, item_key, type, percent, stat));
      }
    }
  }

  JsonValue enchant_slots = BuildEnchantSlots(saved_item, item, all_stats, bonus_stats, bonus_owner, item_key,
                                              exp_bonus, gold_bonus, bonus_sources);

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "uniqueId", CopyOrNull(ObjectGet(saved_item, "UniqueId")));
  ObjectSet(out, "itemKey", CopyOrNull(item_key));
  ObjectSet(out, "name", item ? CopyOrNull(ObjectGet(*item, "name")) : JsonValue::Null());
  ObjectSet(out, "grade", item ? CopyOrNull(ObjectGet(*item, "grade")) : JsonValue::Null());
  ObjectSet(out, "part", item ? CopyOrNull(ObjectGet(*item, "parts")) : JsonValue::Null());
  ObjectSet(out, "icon", item ? CopyOrNull(ObjectGet(*item, "icon")) : JsonValue::Null());
  ObjectSet(out, "level", item ? CopyOrNull(ObjectGet(*item, "level")) : JsonValue::Null());
  ObjectSet(out, "variant", item ? CopyOrNull(ObjectGet(*item, "variant")) : JsonValue::Null());
  ObjectSet(out, "uniqueMod", item ? CopyOrNull(ObjectGet(*item, "uniqueMod")) : JsonValue::Null());
  ObjectSet(out, "slotCapacity", item ? CopyOrNull(ObjectGet(*item, "slots")) : JsonValue::Null());
  ObjectSet(out, "enchantSlots", std::move(enchant_slots));
  ObjectSet(out, "stats", std::move(all_stats));
  ObjectSet(out, "bonusStats", std::move(bonus_stats));

  if (const JsonValue* value = ObjectGet(saved_item, "IsBlocked")) ObjectSet(out, "blocked", *value);
  if (const JsonValue* value = ObjectGet(saved_item, "IsChaotic")) ObjectSet(out, "chaotic", *value);
  if (const JsonValue* value = ObjectGet(saved_item, "IsServerPendingItem")) ObjectSet(out, "serverPending", *value);
  if (const JsonValue* value = ObjectGet(saved_item, "IsViewed")) {
    ObjectSet(out, "viewed", *value);
    ObjectSet(out, "isNew", JsonValue::Bool(!JsonBool(value, true)));
  }
  if (const JsonValue* value = ObjectGet(saved_item, "IsNew")) ObjectSet(out, "isNew", *value);

  return out;
}

const JsonValue* ItemByKey(const std::string& key) {
  // Carrega as definicoes do items.json (extraido do items.zip embutido) uma
  // vez e indexa por key. Antes embutiamos um indice .txt cru de ~2.3MB no exe;
  // reusar o zip (181KB) e descompactar reduz o binario em ~2MB.
  static bool loaded = false;
  static JsonValue items_array;  // mantem o array parseado vivo (ponteiros estaveis)
  static std::map<std::string, const JsonValue*> by_key;

  if (!loaded) {
    loaded = true;
    std::string text;
    if (ReadTextFile(ItemsJsonPath(), text) && ParseJson(text, items_array) &&
        items_array.type == JsonValue::Type::Array) {
      for (const auto& item : items_array.array) {
        std::string k = JsonNumberKey(ObjectGet(item, "key"));
        if (!k.empty()) by_key.emplace(k, &item);
      }
    }
  }

  auto it = by_key.find(key);
  return it == by_key.end() ? nullptr : it->second;
}

JsonValue BuildEquippedItems(const JsonValue& hero,
                             const std::map<std::string, const JsonValue*>& item_by_uid,
                             double& exp_bonus,
                             double& gold_bonus,
                             JsonValue& bonus_sources,
                             bool accumulate_bonuses) {
  JsonValue equipped = JsonValue::Array();
  const JsonValue* equipped_ids = ObjectGet(hero, "equippedItemIds");
  if (!equipped_ids || equipped_ids->type != JsonValue::Type::Array) return equipped;

  double ignored_exp_bonus = 0;
  double ignored_gold_bonus = 0;
  JsonValue ignored_bonus_sources = JsonValue::Array();
  double* target_exp_bonus = accumulate_bonuses ? &exp_bonus : &ignored_exp_bonus;
  double* target_gold_bonus = accumulate_bonuses ? &gold_bonus : &ignored_gold_bonus;
  JsonValue* target_bonus_sources = accumulate_bonuses ? &bonus_sources : &ignored_bonus_sources;

  for (const auto& unique_id : equipped_ids->array) {
    if (IsZeroNumber(&unique_id)) continue;
    auto saved_it = item_by_uid.find(JsonNumberKey(&unique_id));
    if (saved_it == item_by_uid.end()) continue;
    const JsonValue* saved_item = saved_it->second;
    const JsonValue* item_key = ObjectGet(*saved_item, "ItemKey");
    const JsonValue* item = ItemByKey(JsonNumberKey(item_key));
    if (!item) continue;
    equipped.array.push_back(BuildSavedItemSummary(*saved_item, item, item_key, hero, *target_exp_bonus,
                                                   *target_gold_bonus, *target_bonus_sources));
  }
  return equipped;
}

JsonValue BuildHeroSummary(const JsonValue& hero, bool include_equipment,
                           bool accumulate_equipment_bonuses,
                           const std::map<std::string, const JsonValue*>& item_by_uid,
                           double& exp_bonus,
                           double& gold_bonus,
                           JsonValue& bonus_sources) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "heroKey", CopyOrNull(ObjectGet(hero, "heroKey")));
  ObjectSet(out, "level", CopyOrNull(ObjectGet(hero, "HeroLevel")));
  ObjectSet(out, "exp", CopyOrNull(ObjectGet(hero, "HeroExp")));
  ObjectSet(out, "abilityPoint", CopyOrNull(ObjectGet(hero, "AbilityPoint")));
  ObjectSet(out, "allocatedAbilityPoint", CopyOrNull(ObjectGet(hero, "AllocatedHeroAbilityPoint")));
  ObjectSet(out, "equippedSkillKey", CopyOrEmptyArray(ObjectGet(hero, "equippedSKillKey")));
  if (include_equipment) {
    ObjectSet(out, "equippedItemIds", CopyOrEmptyArray(ObjectGet(hero, "equippedItemIds")));
    ObjectSet(out, "equippedItems", BuildEquippedItems(hero, item_by_uid, exp_bonus, gold_bonus, bonus_sources,
                                                       accumulate_equipment_bonuses));
  }
  return out;
}

bool LoadSaveRoot(const std::string& json, JsonValue& save) {
  JsonValue outer;
  if (!ParseJson(json, outer) || outer.type != JsonValue::Type::Object) return false;
  save = JsonValue::Object();
  for (const auto& item : outer.object) {
    const JsonValue* value = ObjectGet(item.second, "value");
    if (!value) value = &item.second;
    if (value->type == JsonValue::Type::String && !value->string.empty() && value->string.front() == '{') {
      JsonValue inner;
      if (ParseJson(value->string, inner)) {
        ObjectSet(save, item.first, std::move(inner));
      } else {
        ObjectSet(save, item.first, *value);
      }
    } else {
      ObjectSet(save, item.first, *value);
    }
  }
  return true;
}

JsonValue BuildLevelMap(const JsonValue* array, const std::string& key_field, const std::string& level_field) {
  JsonValue out = JsonValue::Object();
  if (!array || array->type != JsonValue::Type::Array) return out;
  for (const auto& item : array->array) {
    std::string key = JsonNumberKey(ObjectGet(item, key_field));
    if (!key.empty()) ObjectSet(out, key, CopyOrNull(ObjectGet(item, level_field)));
  }
  return out;
}

JsonValue BuildPetsSummary(const JsonValue* pets) {
  JsonValue out = JsonValue::Array();
  if (!pets || pets->type != JsonValue::Type::Array) return out;
  for (const auto& pet : pets->array) {
    JsonValue item = JsonValue::Object();
    ObjectSet(item, "petKey", CopyOrNull(ObjectGet(pet, "PetKey")));
    ObjectSet(item, "unlocked", CopyOrNull(ObjectGet(pet, "IsUnlock")));
    ObjectSet(item, "viewed", CopyOrNull(ObjectGet(pet, "IsViewed")));
    out.array.push_back(std::move(item));
  }
  return out;
}

JsonValue BuildRunesSummary(const JsonValue* runes) {
  JsonValue out = JsonValue::Array();
  if (!runes || runes->type != JsonValue::Type::Array) return out;
  for (const auto& rune : runes->array) {
    JsonValue item = JsonValue::Object();
    ObjectSet(item, "runeKey", CopyOrNull(ObjectGet(rune, "RuneKey")));
    const JsonValue* level = ObjectGet(rune, "Level");
    ObjectSet(item, "level", level ? *level : JsonValue::Number(0LL));
    out.array.push_back(std::move(item));
  }
  return out;
}

JsonValue BuildMonsterKills(const JsonValue* aggregates) {
  JsonValue out = JsonValue::Object();
  if (!aggregates || aggregates->type != JsonValue::Type::Array) return out;
  for (const auto& row : aggregates->array) {
    if (static_cast<long long>(JsonNumberDouble(ObjectGet(row, "Type"), -1)) != 0) continue;
    std::string key = JsonNumberKey(ObjectGet(row, "SubKey"));
    if (key.empty() || key == "0") continue;
    ObjectSet(out, key, CopyOrNull(ObjectGet(row, "Value")));
  }
  return out;
}

JsonValue GoldQuantity(const JsonValue* currencies) {
  const JsonValue* gold = FindByNumberKey(currencies, "Key", "100001");
  return CopyOrNull(gold ? ObjectGet(*gold, "Quantity") : nullptr);
}

const JsonValue* SlotUnlockField(const JsonValue& slot) {
  const JsonValue* unlocked = ObjectGet(slot, "IsUnlock");
  if (!unlocked) unlocked = ObjectGet(slot, "IsUnLock");
  return unlocked;
}

JsonValue BuildInventorySlotSummary(const JsonValue& slot,
                                    const std::map<std::string, const JsonValue*>& item_by_uid) {
  JsonValue out = JsonValue::Object();
  const JsonValue* item_uid = ObjectGet(slot, "ItemUniqueId");
  const JsonValue* unlocked = SlotUnlockField(slot);
  ObjectSet(out, "index", CopyOrNull(ObjectGet(slot, "Index")));
  ObjectSet(out, "unlocked", unlocked ? *unlocked : JsonValue::Bool(false));
  if (const JsonValue* unlocked_by_rune = ObjectGet(slot, "IsUnlockedByRune")) {
    ObjectSet(out, "unlockedByRune", *unlocked_by_rune);
  }
  ObjectSet(out, "itemUniqueId", CopyOrNull(item_uid));
  ObjectSet(out, "occupied", JsonValue::Bool(!IsZeroNumber(item_uid)));

  auto saved_it = item_by_uid.find(JsonNumberKey(item_uid));
  if (saved_it == item_by_uid.end()) {
    ObjectSet(out, "item", JsonValue::Null());
    return out;
  }

  const JsonValue* saved_item = saved_it->second;
  const JsonValue* item_key = ObjectGet(*saved_item, "ItemKey");
  const JsonValue* item = ItemByKey(JsonNumberKey(item_key));
  JsonValue bonus_owner = JsonValue::Object();
  double ignored_exp_bonus = 0;
  double ignored_gold_bonus = 0;
  JsonValue ignored_bonus_sources = JsonValue::Array();
  ObjectSet(out, "item", BuildSavedItemSummary(*saved_item, item, item_key, bonus_owner, ignored_exp_bonus,
                                               ignored_gold_bonus, ignored_bonus_sources));
  return out;
}

JsonValue BuildInventoryTabSummary(const std::string& id,
                                   const std::string& label,
                                   const JsonValue* source_slots,
                                   const std::map<std::string, const JsonValue*>& item_by_uid) {
  JsonValue tab = JsonValue::Object();
  JsonValue slots = JsonValue::Array();
  long long total = 0;
  long long unlocked_count = 0;
  long long occupied_count = 0;

  if (source_slots && source_slots->type == JsonValue::Type::Array) {
    total = static_cast<long long>(source_slots->array.size());
    for (const auto& slot : source_slots->array) {
      const JsonValue* unlocked = SlotUnlockField(slot);
      bool is_unlocked = JsonBool(unlocked, false);
      bool is_occupied = !IsZeroNumber(ObjectGet(slot, "ItemUniqueId"));
      if (is_unlocked) unlocked_count++;
      if (is_occupied) occupied_count++;
      if (is_unlocked) slots.array.push_back(BuildInventorySlotSummary(slot, item_by_uid));
    }
  }

  ObjectSet(tab, "id", JsonValue::String(id));
  ObjectSet(tab, "label", JsonValue::String(label));
  ObjectSet(tab, "totalSlots", JsonValue::Number(total));
  ObjectSet(tab, "unlockedSlots", JsonValue::Number(unlocked_count));
  ObjectSet(tab, "lockedSlots", JsonValue::Number((std::max)(0LL, total - unlocked_count)));
  ObjectSet(tab, "occupiedSlots", JsonValue::Number(occupied_count));
  ObjectSet(tab, "emptySlots", JsonValue::Number((std::max)(0LL, unlocked_count - occupied_count)));
  ObjectSet(tab, "slots", std::move(slots));
  return tab;
}

JsonValue BuildInventorySummary(const JsonValue* player,
                                const std::map<std::string, const JsonValue*>& item_by_uid) {
  JsonValue out = JsonValue::Object();
  JsonValue tabs = JsonValue::Array();
  tabs.array.push_back(BuildInventoryTabSummary("inventory", "Inventário",
                                                player ? ObjectGet(*player, "inventorySaveDatas") : nullptr,
                                                item_by_uid));
  tabs.array.push_back(BuildInventoryTabSummary("storage", "Storage",
                                                player ? ObjectGet(*player, "stashSaveDatas") : nullptr,
                                                item_by_uid));
  tabs.array.push_back(BuildInventoryTabSummary("trade", "Troca",
                                                player ? ObjectGet(*player, "tradingStashSaveDatas") : nullptr,
                                                item_by_uid));
  ObjectSet(out, "tabs", std::move(tabs));
  return out;
}

JsonValue BuildSaveSummaryJson(const JsonValue& save) {
  const JsonValue* player = ObjectGet(save, "PlayerSaveData");
  const JsonValue* account = ObjectGet(save, "AccountSaveData");
  const JsonValue* common = player ? ObjectGet(*player, "commonSaveData") : nullptr;
  const JsonValue* heroes = player ? ObjectGet(*player, "heroSaveDatas") : nullptr;
  const JsonValue* arranged = common ? ObjectGet(*common, "arrangedHeroKey") : nullptr;
  const JsonValue* pets = player ? ObjectGet(*player, "PetSaveData") : nullptr;
  const JsonValue* runes = player ? ObjectGet(*player, "RuneSaveData") : nullptr;
  const JsonValue* attributes = player ? ObjectGet(*player, "attributeSaveDatas") : nullptr;
  const JsonValue* item_saves = player ? ObjectGet(*player, "itemSaveDatas") : nullptr;
  const JsonValue* aggregates = player ? ObjectGet(*player, "aggregateSaveDatas") : nullptr;

  auto hero_by_key = IndexArrayByNumberKey(heroes, "heroKey");
  auto item_by_uid = IndexArrayByNumberKey(item_saves, "UniqueId");

  double exp_bonus = 0;
  double gold_bonus = 0;
  JsonValue bonus_sources = JsonValue::Array();

  JsonValue party = JsonValue::Array();
  if (arranged && arranged->type == JsonValue::Type::Array) {
    for (const auto& hero_key : arranged->array) {
      auto it = hero_by_key.find(JsonNumberKey(&hero_key));
      if (it != hero_by_key.end()) {
        party.array.push_back(BuildHeroSummary(*it->second, true, true, item_by_uid, exp_bonus, gold_bonus,
                                               bonus_sources));
      }
    }
  }

  JsonValue unlocked = JsonValue::Array();
  if (heroes && heroes->type == JsonValue::Type::Array) {
    for (const auto& hero : heroes->array) {
      if (JsonBool(ObjectGet(hero, "IsUnLock"))) {
        unlocked.array.push_back(BuildHeroSummary(hero, true, false, item_by_uid, exp_bonus, gold_bonus,
                                                 bonus_sources));
      }
    }
  }

  JsonValue equipment_bonuses = JsonValue::Object();
  ObjectSet(equipment_bonuses, "exp", JsonValue::Number(exp_bonus));
  ObjectSet(equipment_bonuses, "gold", JsonValue::Number(gold_bonus));
  ObjectSet(equipment_bonuses, "sources", std::move(bonus_sources));

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "version", CopyOrNull((common && ObjectGet(*common, "version")) ? ObjectGet(*common, "version") : (account ? ObjectGet(*account, "version") : nullptr)));
  const JsonValue* owner = account ? ObjectGet(*account, "ownerSteamId") : nullptr;
  const JsonValue* player_id = account ? ObjectGet(*account, "playerId") : nullptr;
  ObjectSet(out, "steamId", owner && !JsonStringValue(owner).empty() ? *owner : CopyOrNull(player_id));
  ObjectSet(out, "ownerSteamId", CopyOrNull(owner));
  ObjectSet(out, "playerId", CopyOrNull(player_id));
  ObjectSet(out, "currentStageKey", CopyOrNull(common ? ObjectGet(*common, "currentStageKey") : nullptr));
  ObjectSet(out, "currentStageWave", CopyOrNull(common ? ObjectGet(*common, "currentStageWave") : nullptr));
  ObjectSet(out, "maxCompletedStage", CopyOrNull(common ? ObjectGet(*common, "maxCompletedStage") : nullptr));
  ObjectSet(out, "playTime", CopyOrNull(common ? ObjectGet(*common, "playTime") : nullptr));
  ObjectSet(out, "arrangedHeroKey", CopyOrEmptyArray(arranged));
  ObjectSet(out, "arrangedPetKey", CopyOrNull(common ? ObjectGet(*common, "ArrangedPetKey") : nullptr));
  ObjectSet(out, "partyHeroLevels", std::move(party));
  ObjectSet(out, "unlockedHeroes", std::move(unlocked));
  ObjectSet(out, "pets", BuildPetsSummary(pets));
  ObjectSet(out, "attributeLevels", BuildLevelMap(attributes, "Key", "Level"));
  ObjectSet(out, "runeLevels", BuildLevelMap(runes, "RuneKey", "Level"));
  ObjectSet(out, "runes", BuildRunesSummary(runes));
  ObjectSet(out, "monsterKills", BuildMonsterKills(aggregates));
  ObjectSet(out, "equipmentBonuses", std::move(equipment_bonuses));
  ObjectSet(out, "inventory", BuildInventorySummary(player, item_by_uid));
  ObjectSet(out, "gold", GoldQuantity(player ? ObjectGet(*player, "currenySaveDatas") : nullptr));
  return out;
}

std::string LeadingDigits(const std::string& value) {
  std::string digits;
  for (char c : value) {
    if (c < '0' || c > '9') break;
    digits.push_back(c);
  }
  return digits;
}

bool ReadSaveSummary(SaveSummary& summary) {
  std::vector<unsigned char> bytes;
  std::wstring path = DefaultSavePath();
  if (!ReadAllBytes(path, bytes)) return false;

  std::string json;
  if (!DecryptEs3AesCbc(bytes, DEFAULT_ES3_KEY, json)) return false;

  JsonValue save;
  if (!LoadSaveRoot(json, save)) return false;
  JsonValue full_summary = BuildSaveSummaryJson(save);
  summary.summary_json = JsonSerialize(full_summary);

  const JsonValue* account = ObjectGet(save, "AccountSaveData");
  const JsonValue* player = ObjectGet(save, "PlayerSaveData");
  const JsonValue* common = player ? ObjectGet(*player, "commonSaveData") : nullptr;

  summary.owner_steam_id = LeadingDigits(JsonStringValue(account ? ObjectGet(*account, "ownerSteamId") : nullptr));
  summary.player_id = JsonStringValue(account ? ObjectGet(*account, "playerId") : nullptr);
  summary.steam_id = summary.owner_steam_id.empty() ? summary.player_id : summary.owner_steam_id;
  summary.version = JsonStringValue(common ? ObjectGet(*common, "version") : nullptr);
  summary.current_stage_key = static_cast<long long>(JsonNumberDouble(common ? ObjectGet(*common, "currentStageKey") : nullptr));
  summary.current_stage_wave = static_cast<long long>(JsonNumberDouble(common ? ObjectGet(*common, "currentStageWave") : nullptr));
  summary.max_completed_stage = static_cast<long long>(JsonNumberDouble(common ? ObjectGet(*common, "maxCompletedStage") : nullptr));
  summary.arranged_pet_key = static_cast<long long>(JsonNumberDouble(common ? ObjectGet(*common, "ArrangedPetKey") : nullptr));
  const JsonValue* gold = ObjectGet(full_summary, "gold");
  summary.gold = static_cast<long long>(JsonNumberDouble(gold));
  summary.pets_json = JsonSerialize(CopyOrEmptyArray(ObjectGet(full_summary, "pets")));
  summary.runes_json = JsonSerialize(CopyOrEmptyArray(ObjectGet(full_summary, "runes")));
  return !summary.steam_id.empty();
}

std::wstring ReadSteamIdFromSave() {
  SaveSummary summary;
  return ReadSaveSummary(summary) ? Widen(summary.steam_id) : L"";
}

std::string JsonEscape(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default: output += c; break;
    }
  }
  return output;
}

std::string SavePayload(const Config& config) {
  SaveSummary summary;
  bool has_save = ReadSaveSummary(summary);
  std::string difficulty = has_save ? DifficultyFromStageKey(summary.current_stage_key) : "NORMAL";
  MemorySnapshot memory = ReadMemorySnapshot(difficulty);
  std::string steam_raw = config.steam_id.empty() ? (summary.steam_id.empty() ? "default" : summary.steam_id) : Utf8(config.steam_id);
  std::string steam = JsonEscape(steam_raw);
  std::string save = "null";
  if (has_save) {
    save = summary.summary_json;
  }
  std::string pairing_hash = PairingHash(config, steam_raw);
  std::string pairing = pairing_hash.empty() ? "null" : ("\"" + pairing_hash + "\"");
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam + "\",\"save\":" + save +
         ",\"pairingHash\":" + pairing +
         ",\"clears\":" + memory.clears_json +
         ",\"history\":" + memory.history_json +
         ",\"watcher\":" + memory.watcher_json + "}";
}

std::string CachedPayload(const Config& config, const WorkerState& state, int events_from_index = -1) {
  std::string steam_raw = EffectiveSteamId(config, state);
  std::string steam = JsonEscape(steam_raw);
  std::string save = state.has_save ? state.save.summary_json : "null";
  std::string pairing_hash = PairingHash(config, steam_raw);
  std::string pairing = pairing_hash.empty() ? "null" : ("\"" + pairing_hash + "\"");
  std::string history = state.memory.history_json;
  if (events_from_index > 0) {
    std::string difficulty = state.has_save ? DifficultyFromStageKey(state.save.current_stage_key) : "NORMAL";
    history = JsonSerialize(BuildHistoryJson(state.memory.events, difficulty, events_from_index));
  }
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam + "\",\"save\":" + save +
         ",\"pairingHash\":" + pairing +
         ",\"clears\":" + state.memory.clears_json +
         ",\"history\":" + history +
         ",\"watcher\":" + state.memory.watcher_json + "}";
}

bool ParseUrl(const std::wstring& url, URL_COMPONENTSW& parts, std::vector<wchar_t>& host, std::vector<wchar_t>& path) {
  host.assign(256, L'\0');
  path.assign(2048, L'\0');
  ZeroMemory(&parts, sizeof(parts));
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host.data();
  parts.dwHostNameLength = static_cast<DWORD>(host.size());
  parts.lpszUrlPath = path.data();
  parts.dwUrlPathLength = static_cast<DWORD>(path.size());
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  return WinHttpCrackUrl(url.c_str(), 0, 0, &parts) == TRUE;
}

std::wstring PostJsonPayload(const Config& config, const std::string& payload) {
  if (config.server.empty()) return L"Configure a URL do servidor.";
  if (config.token.empty()) return L"Configure o token de ingest.";

  std::wstring endpoint = config.server;
  while (!endpoint.empty() && endpoint.back() == L'/') endpoint.pop_back();
  endpoint += L"/api/ingest";

  URL_COMPONENTSW parts{};
  std::vector<wchar_t> host;
  std::vector<wchar_t> path;
  if (!ParseUrl(endpoint, parts, host, path)) return L"URL invalida.";

  std::wstring host_name(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring object_path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength) object_path += std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

  HINTERNET session = WinHttpOpen(L"TBH Companion Agent/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) return L"Falha ao abrir WinHTTP.";
  WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);

  HINTERNET connect = WinHttpConnect(session, host_name.c_str(), parts.nPort, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    return L"Falha ao conectar.";
  }

  DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET request = WinHttpOpenRequest(connect, L"POST", object_path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return L"Falha ao criar request.";
  }

  std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + config.token + L"\r\n";

  BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                               const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
                               static_cast<DWORD>(payload.size()), 0);
  if (ok) ok = WinHttpReceiveResponse(request, nullptr);

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

  std::string body;
  if (ok) {
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available) {
      std::string chunk(available, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
      chunk.resize(read);
      body += chunk;
    }
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);

  if (!ok) return L"POST falhou.";
  return L"HTTP " + std::to_wstring(status) + L" " + Widen(body);
}

std::wstring PostIngest(const Config& config) {
  SaveSummary summary;
  if (!ReadSaveSummary(summary)) return L"Não foi possível ler o save C++.";
  return PostJsonPayload(config, SavePayload(config));
}

bool RefreshSaveCache(WorkerState& state) {
  FILETIME mtime{};
  std::wstring path = DefaultSavePath();
  if (!FileMTime(path, mtime)) return false;
  if (state.has_save && SameFileTime(state.save_mtime, mtime)) return false;
  PostStatus(L"Lendo save...");
  SaveSummary summary;
  if (!ReadSaveSummary(summary)) {
    PostStatus(L"Falha ao ler save.");
    return false;
  }
  state.save = std::move(summary);
  state.save_mtime = mtime;
  state.has_save = true;
  PostStatus(L"Save lido.");
  return true;
}

std::string WatcherStatusJson(const std::string& message) {
  JsonValue watcher = JsonValue::Object();
  ObjectSet(watcher, "source", JsonValue::String("cpp-agent"));
  ObjectSet(watcher, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  ObjectSet(watcher, "message", JsonValue::String(message));
  return JsonSerialize(watcher);
}

bool RefreshMemoryCache(WorkerState& state, bool force_discover = false) {
  DWORD now = GetTickCount();
  DWORD pid = FindProcessIdByName(L"TaskBarHero.exe");
  std::string difficulty = state.has_save ? DifficultyFromStageKey(state.save.current_stage_key) : "NORMAL";
  bool process_changed = pid != state.memory_pid;
  bool loaded_cache = false;
  if (process_changed) {
    state.memory_pid = pid;
    state.memory_seen.clear();
    std::string cache_difficulty = state.has_save ? MaxSaveDifficulty(state.save) : difficulty;
    long long max_completed_stage = state.has_save ? SaveMaxCompletedStage(state.save) : 0;
    if (pid && state.memory.events.empty() && LoadMemoryHistoryCache(state.memory, cache_difficulty, max_completed_stage)) {
      loaded_cache = true;
      PostStatus(L"Historico local carregado (" + std::to_wstring(state.memory.events.size()) + L" eventos).");
    }
    for (const auto& event : state.memory.events) state.memory_seen.insert(event.id);
  }

  if (!pid) return false;
  bool due = force_discover || now - state.last_memory_scan > 2000;
  if (!due) return false;
  state.last_memory_scan = now;

  // Leitura direta da List<LogData> do LogManager via mapa IL2CPP: ordem de
  // insercao garantida pelo proprio jogo e DateTime real por evento. Leva
  // milissegundos (4 ponteiros + array), nada de varrer heap.
  DWORD scan_start = GetTickCount();
  std::vector<MemoryEvent> scanned;
  std::string error;
  if (!ReadLogManagerEvents(pid, scanned, &error)) {
    PostStatus(L"Leitura do log falhou: " + Widen(error));
    return false;
  }

  size_t added = 0;
  for (auto& event : scanned) {
    if (!state.memory_seen.insert(event.id).second) continue;
    if (IsStageEvent(event) && event.difficulty.empty()) event.difficulty = difficulty;
    event.index = static_cast<int>(state.memory.events.size());
    state.memory.events.push_back(std::move(event));
    ++added;
  }
  bool changed = loaded_cache;
  if (added > 0 || state.memory.history_json == "null") {
    RebuildMemorySnapshotJson(state.memory, difficulty);
    SaveMemoryHistoryCache(state.memory);
    changed = true;
  }
  state.memory.pid = pid;
  state.memory.watcher_json = WatcherStatusJson("cpp-agent il2cpp reader");

  DWORD scan_ms = GetTickCount() - scan_start;
  if (added > 0) {
    PostStatus(std::to_wstring(added) + L" evento(s) novo(s) em " + std::to_wstring(scan_ms) + L"ms (" +
               std::to_wstring(state.memory.events.size()) + L" no historico).");
  }
  return changed;
}

std::wstring DevRuntimeDir() {
  std::vector<wchar_t> env(32768);
  DWORD length = GetEnvironmentVariableW(L"TBH_DEV_RUNTIME_DIR", env.data(), static_cast<DWORD>(env.size()));
  if (length > 0 && length < env.size()) return std::wstring(env.data(), length);

  std::wstring cursor = ParentDir(ExePath());
  for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
    std::wstring candidate = cursor + L"\\tbh-farm-local";
    if (DirectoryExists(candidate)) return candidate + L"\\runtime";
    std::wstring parent = ParentDir(cursor);
    if (parent.empty() || parent == cursor) break;
    cursor = parent;
  }
  return CompanionDir() + L"\\runtime";
}

bool WriteDevRuntimeJson(const std::wstring& runtime_dir,
                         const wchar_t* file_name,
                         const std::string& json,
                         std::wstring& error) {
  std::wstring path = runtime_dir + L"\\" + file_name;
  if (WriteUtf8TextFileAtomic(path, json)) return true;
  error = L"Falha ao escrever " + path;
  return false;
}

bool WriteDevRuntimeFiles(const SaveSummary& save,
                          const MemorySnapshot& memory,
                          const std::wstring& runtime_dir,
                          std::wstring& error) {
  if (!EnsureDirectoryTree(runtime_dir)) {
    error = L"Falha ao criar pasta runtime dev: " + runtime_dir;
    return false;
  }
  return WriteDevRuntimeJson(runtime_dir, L"save-summary.json", save.summary_json, error) &&
         WriteDevRuntimeJson(runtime_dir, L"clears.json", memory.clears_json, error) &&
         WriteDevRuntimeJson(runtime_dir, L"log-history.json", memory.history_json, error) &&
         WriteDevRuntimeJson(runtime_dir, L"watcher-status.json", memory.watcher_json, error);
}

std::wstring WriteDevRuntimeSnapshot(const SaveSummary& save, const MemorySnapshot& memory) {
  std::wstring runtime_dir = DevRuntimeDir();
  std::wstring error;
  if (!WriteDevRuntimeFiles(save, memory, runtime_dir, error)) return error;
  return L"OK: runtime dev atualizado em " + runtime_dir + L" (" +
         std::to_wstring(memory.events.size()) + L" evento(s)).";
}

std::wstring ExportDevRuntimeSnapshot() {
  if (!kDevelopmentMode) return L"Export dev indisponível neste build.";

  SaveSummary summary;
  if (!ReadSaveSummary(summary)) return L"Falha: não foi possível ler o save do jogo.";

  WorkerState state;
  state.save = std::move(summary);
  state.has_save = true;
  state.memory.watcher_json = WatcherStatusJson("manual dev export starting");

  RefreshMemoryCache(state, true);

  std::string difficulty = DifficultyFromStageKey(state.save.current_stage_key);
  if (state.memory.history_json == "null" || state.memory.clears_json == "null") {
    MemorySnapshot cached;
    if (LoadMemoryHistoryCache(cached, MaxSaveDifficulty(state.save), SaveMaxCompletedStage(state.save))) state.memory = std::move(cached);
  }

  if (state.memory.history_json == "null" || state.memory.clears_json == "null") {
    return L"Falha: não foi possível ler o histórico. Abra o TaskBarHero.exe e tente de novo.";
  }

  state.memory.watcher_json =
      WatcherStatusJson(state.memory.pid ? "manual dev export from live game" : "manual dev export from cached history");
  SaveMemoryHistoryCache(state.memory);

  return WriteDevRuntimeSnapshot(state.save, state.memory);
}

void AutoExportDevRuntimeSnapshot(const WorkerState& state, bool changed) {
  if (!kDevelopmentMode || !changed || !state.has_save) return;
  if (state.memory.history_json == "null" || state.memory.clears_json == "null") return;
  static std::string last_signature;
  const std::vector<MemoryEvent>& events = state.memory.events;
  std::string signature = Fnv1aHash(state.save.summary_json + "|" +
                                    std::to_string(events.size()) + "|" +
                                    (events.empty() ? "" : events.back().id) + "|" +
                                    state.memory.clears_json + "|" +
                                    state.memory.history_json);
  if (signature == last_signature) return;
  std::wstring result = WriteDevRuntimeSnapshot(state.save, state.memory);
  if (result.rfind(L"OK:", 0) == 0) {
    last_signature = signature;
    PostStatus(L"Save local dev atualizado (" + std::to_wstring(events.size()) + L" evento(s)).");
  } else {
    PostStatus(result);
  }
}

bool SyncCachedPayload(const Config& config, WorkerState& state, bool force = false) {
  const std::vector<MemoryEvent>& events = state.memory.events;
  // Assinatura estavel do conteudo (sem updatedAt): so envia quando o save
  // muda ou quando ha eventos novos.
  std::string steam_raw = EffectiveSteamId(config, state);
  std::string pairing_hash = PairingHash(config, steam_raw);
  std::string signature = Fnv1aHash((state.has_save ? state.save.summary_json : "null") + "|" +
                                    std::to_string(events.size()) + "|" +
                                    (events.empty() ? "" : events.back().id) + "|" +
                                    pairing_hash);
  if (!force && signature == state.last_payload_hash) return false;

  if (config.server.empty() || config.token.empty()) {
    state.last_payload_hash = signature;
    PostStatus(L"Dados atualizados localmente. Configure servidor/token para sync.");
    return false;
  }

  // Envio incremental: manda apenas eventos com index > synced_index. Se o
  // historico local foi resetado/reordenado (firstId mudou), reenvia tudo.
  int from_index = 0;
  if (state.synced_index >= 0 && !events.empty() && state.synced_first_id == events.front().id) {
    from_index = static_cast<int>((std::min)(state.synced_index + 1, static_cast<long long>(events.size())));
  }
  long long new_events = static_cast<long long>(events.size()) - from_index;

  PostStatus(L"Montando payload...");
  std::string payload = CachedPayload(config, state, from_index);
  PostStatus(L"Enviando sync (" + std::to_wstring(payload.size()) + L" bytes, " +
             std::to_wstring(new_events) + L" eventos novos)...");
  std::wstring result = PostJsonPayload(config, payload);
  if (result.rfind(L"HTTP 200", 0) == 0 || result.rfind(L"HTTP 201", 0) == 0) {
    state.last_payload_hash = signature;
    state.synced_index = events.empty() ? -1 : events.back().index;
    state.synced_first_id = events.empty() ? "" : events.front().id;
    SaveSyncState(state);
    PostStatus(L"Sync OK (" + std::to_wstring(new_events) + L" eventos novos enviados).");
    return true;
  }

  PostStatus(L"Sync falhou: " + result);
  return false;
}

bool EnsureGameRunning() {
  PostStatus(L"Verificando jogo...");
  DWORD pid = FindProcessIdByName(L"TaskBarHero.exe");
  if (pid) {
    PostStatus(L"Jogo em execução.");
    return true;
  }

  PostStatus(L"Jogo não encontrado. Abrindo pelo Steam...");
  HINSTANCE result = ShellExecuteW(nullptr, L"open", STEAM_GAME_URI, nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    PostStatus(L"Não foi possível abrir o jogo pelo Steam.");
    return false;
  }

  const DWORD started = GetTickCount();
  while (WaitForSingleObject(g_worker_stop, 2000) == WAIT_TIMEOUT) {
    pid = FindProcessIdByName(L"TaskBarHero.exe");
    if (pid) {
      PostStatus(L"Jogo aberto. Iniciando monitoramento.");
      return true;
    }
    if (GetTickCount() - started > 120000) {
      PostStatus(L"Steam chamado, mas o jogo não iniciou em 120s.");
      return false;
    }
    PostStatus(L"Aguardando o jogo iniciar...");
  }
  return false;
}

DWORD WINAPI WorkerProc(LPVOID) {
  WorkerState state;
  state.memory.watcher_json = WatcherStatusJson("worker starting");
  LoadSyncState(state);
  PostStatus(L"Worker iniciado. Monitorando save e memoria.");
  RefreshSaveCache(state);
  bool game_ready = EnsureGameRunning();
  bool first_sync_done = false;

  while (WaitForSingleObject(g_worker_stop, 1000) == WAIT_TIMEOUT) {
    Config config = LoadConfig();
    bool save_changed = RefreshSaveCache(state);
    bool changed = save_changed;

    if (!game_ready) {
      game_ready = EnsureGameRunning();
      if (!game_ready) {
        WaitForSingleObject(g_worker_stop, 10000);
        continue;
      }
    }

    if (!FindProcessIdByName(L"TaskBarHero.exe")) {
      game_ready = false;
      state.memory_regions.clear();
      state.memory_pid = 0;
      PostStatus(L"Jogo fechado. Aguardando reabrir.");
      continue;
    }

    if (!state.has_save) {
      PostStatus(L"Aguardando save valido.");
      continue;
    }
    if (config.steam_id.empty() && !state.save.steam_id.empty()) {
      config.steam_id = Widen(state.save.steam_id);
      SaveConfig(config);
    }

    std::string sync_config_signature = SyncConfigSignature(config, state);
    bool sync_config_changed = first_sync_done && sync_config_signature != state.last_sync_config_signature;

    if (!first_sync_done || changed || sync_config_changed) {
      first_sync_done = true;
      SyncCachedPayload(config, state, true);
      state.last_sync_config_signature = sync_config_signature;
    }

    bool memory_changed = RefreshMemoryCache(state);
    changed = memory_changed || changed;
    AutoExportDevRuntimeSnapshot(state, save_changed || memory_changed);
    if (memory_changed) {
      SyncCachedPayload(config, state);
    }
  }
  PostStatus(L"Worker parado.");
  return 0;
}

void StartWorker() {
  if (g_worker_thread) return;
  g_worker_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_worker_stop) return;
  g_worker_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
}

void StopWorker() {
  if (g_worker_stop) SetEvent(g_worker_stop);
  if (g_worker_thread) {
    DWORD wait = WaitForSingleObject(g_worker_thread, 5000);
    if (wait == WAIT_OBJECT_0) {
      CloseHandle(g_worker_thread);
      g_worker_thread = nullptr;
    }
  }
  if (!g_worker_thread && g_worker_stop) {
    CloseHandle(g_worker_stop);
    g_worker_stop = nullptr;
  }
}

HFONT CreateUiFont(int size, int weight = FW_NORMAL) {
  return CreateFontW(size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void AddLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
  HWND label = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, g_instance, nullptr);
  SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND AddEdit(HWND parent, int id, const std::wstring& value, int x, int y, int w, int h, HFONT font, bool password = false) {
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
  SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  if (password) SendMessageW(edit, EM_SETPASSWORDCHAR, L'*', 0);
  return edit;
}

HWND AddButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
  HWND button = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
  SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return button;
}

HWND AddCheckbox(HWND parent, int id, const wchar_t* text, bool checked, int x, int y, int w, int h, HFONT font) {
  HWND checkbox = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
  SendMessageW(checkbox, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  SendMessageW(checkbox, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  return checkbox;
}

void OpenWebUi(const Config& config) {
  std::wstring url = config.server.empty() ? L"https://tbh.gided.in" : config.server;
  if (!config.steam_id.empty()) {
    url += (url.find(L'?') == std::wstring::npos ? L"?steamid=" : L"&steamid=");
    url += config.steam_id;
  }
  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  static HFONT font = nullptr;
  static HFONT title_font = nullptr;
  static HBRUSH bg_brush = nullptr;

  switch (message) {
    case WM_CREATE: {
      g_main_window = hwnd;
      font = CreateUiFont(18);
      title_font = CreateUiFont(26, FW_BOLD);
      Config config = LoadConfig();

      AddLabel(hwnd, L"TBH Companion", 20, 18, 420, 34, title_font);
      AddLabel(hwnd, L"Configuração do worker local", 20, 52, 420, 22, font);

      AddLabel(hwnd, L"Servidor", 24, 96, 120, 22, font);
      g_server = AddEdit(hwnd, IDC_SERVER, config.server, 144, 92, 430, 28, font);

      AddLabel(hwnd, L"Token", 24, 136, 120, 22, font);
      g_token = AddEdit(hwnd, IDC_TOKEN, config.token, 144, 132, 430, 28, font, true);

      AddLabel(hwnd, L"SteamID", 24, 176, 120, 22, font);
      g_steam = AddEdit(hwnd, IDC_STEAM, config.steam_id, 144, 172, 430, 28, font);

      AddLabel(hwnd, L"Chave", 24, 216, 120, 22, font);
      g_pairing_secret = AddEdit(hwnd, IDC_PAIRING_SECRET, config.pairing_secret, 144, 212, 430, 28, font, true);

      g_autostart = AddCheckbox(hwnd, IDC_AUTOSTART, L"Iniciar com Windows", config.auto_start, 144, 250, 220, 24, font);

      AddButton(hwnd, IDC_OPEN, L"Abrir UI", 144, 290, 120, 34, font);
      if (kDevelopmentMode) {
        AddButton(hwnd, IDC_EXPORT_DEV_RUNTIME, L"Atualizar dev", 276, 290, 150, 34, font);
      }

      g_status = CreateWindowW(L"STATIC", L"Pronto.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               24, 350, 550, 70, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
      SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

      if (config.steam_id.empty()) {
        std::wstring steam_id = ReadSteamIdFromSave();
        if (!steam_id.empty()) {
          SetWindowTextW(g_steam, steam_id.c_str());
          config.steam_id = steam_id;
          SaveConfig(config);
          SetStatus(L"SteamID lido automaticamente do save: " + steam_id);
        }
      }
      StartWorker();
      AddTrayIcon(hwnd);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wparam);
      int notification = HIWORD(wparam);
      if ((id == IDC_SERVER || id == IDC_TOKEN || id == IDC_STEAM || id == IDC_PAIRING_SECRET) && notification == EN_CHANGE) {
        SaveConfigFromUi();
      } else if (id == IDC_OPEN) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        OpenWebUi(config);
      } else if (id == IDC_EXPORT_DEV_RUNTIME) {
        SaveConfig(ReadConfigFromUi());
        SetStatus(L"Exportando runtime dev...");
        SetStatus(ExportDevRuntimeSnapshot());
      } else if (id == IDC_AUTOSTART) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        SetStatus(config.auto_start ? L"Inicialização com Windows ativada." : L"Inicialização com Windows desativada.");
      } else if (id == IDM_TRAY_OPEN) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
      } else if (id == IDM_TRAY_WEB) {
        OpenWebUi(LoadConfig());
      } else if (id == IDM_TRAY_TOGGLE_TBH) {
        ToggleGameWindowVisibility();
      } else if (id == IDM_TRAY_MINIMIZE_TO_TASKBAR) {
        Config config = LoadConfig();
        config.minimize_to_taskbar = !config.minimize_to_taskbar;
        SaveConfig(config);
        SetStatus(config.minimize_to_taskbar ? L"Minimizar para taskbar ativado." : L"Minimizar para tray ativado.");
      } else if (id == IDM_TRAY_EXPORT_DEV_RUNTIME && kDevelopmentMode) {
        SaveConfig(ReadConfigFromUi());
        SetStatus(L"Atualizando save local...");
        SetStatus(ExportDevRuntimeSnapshot());
      } else if (id == IDM_TRAY_EXIT) {
        DestroyWindow(hwnd);
      }
      return 0;
    }
    case WM_APP_STATUS: {
      auto* text = reinterpret_cast<std::wstring*>(lparam);
      if (text) {
        SetStatus(*text);
        delete text;
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, RGB(225, 232, 228));
      SetBkColor(dc, RGB(23, 26, 29));
      if (!bg_brush) bg_brush = CreateSolidBrush(RGB(23, 26, 29));
      return reinterpret_cast<LRESULT>(bg_brush);
    }
    case WM_APP_TRAY: {
      UINT event = static_cast<UINT>(LOWORD(lparam));
      if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
      } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        ShowTrayMenu(hwnd);
      }
      return 0;
    }
    case WM_SIZE:
      if (wparam == SIZE_MINIMIZED && !LoadConfig().minimize_to_taskbar) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      break;
    case WM_CLOSE: {
      // Fechar a janela apenas esconde; o worker continua na bandeja.
      static bool hint_shown = false;
      ShowWindow(hwnd, SW_HIDE);
      if (!hint_shown) {
        ShowTrayBalloon(L"TBH Companion", L"Continuo rodando em segundo plano. Clique com o botão direito no ícone para sair.");
        hint_shown = true;
      }
      return 0;
    }
    case WM_DESTROY:
      RemoveTrayIcon();
      StopWorker();
      if (font) DeleteObject(font);
      if (title_font) DeleteObject(title_font);
      if (bg_brush) DeleteObject(bg_brush);
      g_main_window = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  if (g_taskbar_created_msg && message == g_taskbar_created_msg) {
    // Explorer reiniciou: recoloca o icone na bandeja.
    AddTrayIcon(hwnd);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  g_instance = instance;

  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    for (int i = 1; i < argc; ++i) {
      if (wcscmp(argv[i], L"--compare") == 0) {
        SaveSummary summary;
        std::wstring result;
        if (!ReadSaveSummary(summary)) {
          result = L"FAIL: não foi possível ler o save C++";
        } else {
          JsonValue parsed;
          result = ParseJson(summary.summary_json, parsed)
                       ? L"OK: resumo C++ válido. steamId=" + Widen(summary.steam_id) +
                             L" stage=" + std::to_wstring(summary.current_stage_key)
                       : L"FAIL: resumo C++ não é JSON válido.";
        }
        WriteTextFile(TempComparePath(), result);
        LocalFree(argv);
        return result.rfind(L"OK:", 0) == 0 ? 0 : 2;
      }
      if (wcscmp(argv[i], L"--dump-save-summary") == 0) {
        SaveSummary summary;
        std::wstring output = TempSaveSummaryPath();
        if (i + 1 < argc && argv[i + 1][0] != L'-') output = argv[++i];
        bool ok = ReadSaveSummary(summary) && WriteUtf8TextFileAtomic(output, summary.summary_json);
        if (!ok) WriteTextFile(TempComparePath(), L"FAIL: não foi possível exportar o resumo C++.");
        LocalFree(argv);
        return ok ? 0 : 2;
      }
      if (wcscmp(argv[i], L"--export-dev-runtime") == 0) {
        std::wstring result = ExportDevRuntimeSnapshot();
        WriteTextFile(TempComparePath(), result);
        LocalFree(argv);
        return result.rfind(L"OK:", 0) == 0 ? 0 : 2;
      }
      if (wcscmp(argv[i], L"--memory-scan") == 0) {
        MemorySnapshot snapshot = ReadMemorySnapshot("NORMAL");
        JsonValue out = JsonValue::Object();
        ObjectSet(out, "pid", JsonValue::Number(static_cast<long long>(snapshot.pid)));
        ObjectSet(out, "events", JsonValue::Number(static_cast<long long>(snapshot.events.size())));
        JsonValue history;
        JsonValue clears;
        JsonValue watcher;
        ParseJson(snapshot.history_json, history);
        ParseJson(snapshot.clears_json, clears);
        ParseJson(snapshot.watcher_json, watcher);
        ObjectSet(out, "history", history);
        ObjectSet(out, "clears", clears);
        ObjectSet(out, "watcher", watcher);
        WriteTextFile(TempComparePath(), Widen(JsonSerialize(out)));
        LocalFree(argv);
        return snapshot.pid ? 0 : 2;
      }
    }
    LocalFree(argv);
  }

  HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
  if (!instance_mutex) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindWindowW(APP_WINDOW_CLASS, nullptr);
    if (existing) {
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    }
    CloseHandle(instance_mutex);
    return 0;
  }

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&controls);

  g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = APP_WINDOW_CLASS;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
  wc.hbrBackground = CreateSolidBrush(RGB(23, 26, 29));
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0, APP_WINDOW_CLASS, L"TBH Companion",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 620, 470,
                              nullptr, nullptr, instance, nullptr);
  if (!hwnd) {
    CloseHandle(instance_mutex);
    return 1;
  }

  HICON small_icon = reinterpret_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  HICON big_icon = reinterpret_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
  if (small_icon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
  if (big_icon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));

  // Ja configurado: inicia direto na bandeja. Primeira execucao mostra a janela.
  Config startup_config = LoadConfig();
  bool start_hidden = !startup_config.token.empty() || !startup_config.steam_id.empty();
  if (start_hidden) {
    ShowTrayBalloon(L"TBH Companion", L"Rodando em segundo plano. Clique no ícone da bandeja para abrir.");
  } else {
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CloseHandle(instance_mutex);
  return 0;
}
