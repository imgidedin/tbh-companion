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

namespace {

constexpr int IDC_SERVER = 1001;
constexpr int IDC_TOKEN = 1002;
constexpr int IDC_STEAM = 1003;
constexpr int IDC_OPEN = 1006;
constexpr int IDC_STATUS = 1007;
constexpr int IDC_AUTOSTART = 1009;
constexpr UINT WM_APP_STATUS = WM_APP + 1;

constexpr wchar_t DEFAULT_SAVE_RELATIVE[] = L"\\AppData\\LocalLow\\TesseractStudio\\TaskbarHero\\SaveFile_Live.es3";
constexpr char DEFAULT_ES3_KEY[] = "emuMqG3bLYJ938ZDCfieWJ";
constexpr wchar_t STEAM_GAME_URI[] = L"steam://rungameid/3678970";
constexpr wchar_t AUTOSTART_VALUE_NAME[] = L"TBH Companion";

HINSTANCE g_instance = nullptr;
HWND g_server = nullptr;
HWND g_token = nullptr;
HWND g_steam = nullptr;
HWND g_autostart = nullptr;
HWND g_status = nullptr;
HWND g_main_window = nullptr;
HANDLE g_worker_stop = nullptr;
HANDLE g_worker_thread = nullptr;

std::string Utf8(const std::wstring& text);
std::wstring CompanionDir();

struct Config {
  std::wstring server = L"https://tbh.gided.in";
  std::wstring token;
  std::wstring steam_id;
  bool auto_start = false;
};

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
  int seconds = 0;
  std::string progress;
  std::string item;
  std::string hero;
  std::string enemy;
  std::string clock;
  std::string raw;
  std::string id;
  int index = 0;
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

void SetStatus(const std::wstring& text) {
  SetWindowTextW(g_status, text.c_str());
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
  config.auto_start = IsAutoStartEnabled();

  return config;
}

void SaveConfig(const Config& config) {
  std::wstring path = ConfigPath();
  WritePrivateProfileStringW(L"server", L"url", config.server.c_str(), path.c_str());
  WritePrivateProfileStringW(L"server", L"token", config.token.c_str(), path.c_str());
  WritePrivateProfileStringW(L"player", L"steam_id", config.steam_id.c_str(), path.c_str());
  WritePrivateProfileStringW(L"app", L"auto_start", config.auto_start ? L"1" : L"0", path.c_str());
  SetAutoStart(config.auto_start);
}

Config ReadConfigFromUi() {
  Config config;
  config.server = GetWindowTextString(g_server);
  config.token = GetWindowTextString(g_token);
  config.steam_id = GetWindowTextString(g_steam);
  config.auto_start = g_autostart && SendMessageW(g_autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
  return config;
}

bool UiConfigReady() {
  return g_server && g_token && g_steam && g_autostart;
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

std::string EventId(const MemoryEvent& event) {
  if (event.type == "clear") return "clear|" + event.label + "|" + std::to_string(event.seconds) + "|" + event.clock + "|" + event.raw;
  if (event.type == "failure") return "failure|" + event.label + "|" + event.progress + "|" + event.clock + "|" + event.raw;
  if (event.type == "death") return "death|" + event.hero + "|" + event.enemy + "|" + event.clock + "|" + event.raw;
  if (event.type == "drop") return "drop|" + event.item + "|" + event.clock + "|" + event.raw;
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

  auto add_matches = [&](const std::wregex& regex, const std::string& type) {
    for (std::wsregex_iterator it(text.begin(), text.end(), regex), end; it != end; ++it) {
      const std::wsmatch& match = *it;
      MemoryEvent event;
      event.type = type;
      event.raw = Utf8(match.str(0));
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
      events.push_back(std::move(event));
    }
  };

  if (mask & kEventClear) add_matches(clear_re, "clear");
  if (mask & kEventFailure) add_matches(fail_re, "failure");
  if (mask & kEventDeath) add_matches(death_re, "death");
  if (mask & kEventDrop) add_matches(drop_re, "drop");
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
    return UniqueEvents(events);
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
  return UniqueEvents(events);
}

JsonValue EventJson(const MemoryEvent& event) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "type", JsonValue::String(event.type));
  if (!event.label.empty()) ObjectSet(out, "label", JsonValue::String(event.label));
  if (event.type == "clear") ObjectSet(out, "seconds", JsonValue::Number(static_cast<long long>(event.seconds)));
  if (!event.progress.empty()) ObjectSet(out, "progress", JsonValue::String(event.progress));
  if (!event.item.empty()) ObjectSet(out, "item", JsonValue::String(event.item));
  if (!event.hero.empty()) ObjectSet(out, "hero", JsonValue::String(event.hero));
  if (!event.enemy.empty()) ObjectSet(out, "enemy", JsonValue::String(event.enemy));
  ObjectSet(out, "clock", JsonValue::String(event.clock));
  ObjectSet(out, "raw", JsonValue::String(event.raw));
  ObjectSet(out, "id", JsonValue::String(event.id));
  ObjectSet(out, "index", JsonValue::Number(static_cast<long long>(event.index)));
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

std::string DifficultyFromStageKey(long long stage_key) {
  long long tier = stage_key / 1000;
  if (tier == 4) return "TORMENT";
  if (tier == 3) return "HELL";
  if (tier == 2) return "NIGHTMARE";
  return "NORMAL";
}

JsonValue BuildHistoryJson(const std::vector<MemoryEvent>& events, const std::string& difficulty) {
  struct StageGroup {
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
      by_stage[event.label].clears.push_back(event);
    } else if (event.type == "failure") {
      ++total_failures;
      by_stage[event.label].failures.push_back(event);
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

  std::vector<std::string> labels;
  for (const auto& item : by_stage) labels.push_back(item.first);
  std::sort(labels.begin(), labels.end(), StageLabelLess);

  JsonValue stage_summaries = JsonValue::Array();
  for (const std::string& label : labels) {
    const StageGroup& group = by_stage[label];
    std::vector<int> clear_seconds;
    for (const auto& event : group.clears) clear_seconds.push_back(event.seconds);
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "label", JsonValue::String(label));
    ObjectSet(row, "difficulty", JsonValue::String(difficulty));
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
  for (const auto& event : events) event_json.array.push_back(EventJson(event));

  JsonValue totals = JsonValue::Object();
  ObjectSet(totals, "clears", JsonValue::Number(static_cast<long long>(total_clears)));
  ObjectSet(totals, "failures", JsonValue::Number(static_cast<long long>(total_failures)));
  ObjectSet(totals, "deaths", JsonValue::Number(static_cast<long long>(total_deaths)));
  ObjectSet(totals, "drops", JsonValue::Number(static_cast<long long>(total_drops)));

  JsonValue out = JsonValue::Object();
  ObjectSet(out, "source", JsonValue::String("memory"));
  ObjectSet(out, "kind", JsonValue::String("log-history"));
  ObjectSet(out, "difficulty", JsonValue::String(difficulty));
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
  std::map<std::string, std::vector<int>> by_stage;
  const MemoryEvent* last_clear = nullptr;
  for (const auto& event : events) {
    if (event.type != "clear") continue;
    by_stage[event.label].push_back(event.seconds);
    last_clear = &event;
  }
  std::vector<std::string> labels;
  for (const auto& item : by_stage) labels.push_back(item.first);
  std::sort(labels.begin(), labels.end(), StageLabelLess);

  JsonValue averages = JsonValue::Array();
  for (const auto& label : labels) {
    const std::vector<int>& values = by_stage[label];
    if (values.empty()) continue;
    size_t start = values.size() > 10 ? values.size() - 10 : 0;
    int sum = 0;
    JsonValue recent = JsonValue::Array();
    for (size_t i = start; i < values.size(); ++i) {
      sum += values[i];
      recent.array.push_back(JsonValue::Number(static_cast<long long>(values[i])));
    }
    JsonValue row = JsonValue::Object();
    ObjectSet(row, "label", JsonValue::String(label));
    ObjectSet(row, "difficulty", JsonValue::String(difficulty));
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
  event.seconds = static_cast<int>(JsonNumberDouble(ObjectGet(value, "seconds")));
  event.progress = JsonStringValue(ObjectGet(value, "progress"));
  event.item = JsonStringValue(ObjectGet(value, "item"));
  event.hero = JsonStringValue(ObjectGet(value, "hero"));
  event.enemy = JsonStringValue(ObjectGet(value, "enemy"));
  event.clock = JsonStringValue(ObjectGet(value, "clock"));
  event.raw = JsonStringValue(ObjectGet(value, "raw"));
  event.id = JsonStringValue(ObjectGet(value, "id"));
  if (event.id.empty()) event.id = EventId(event);
  event.index = static_cast<int>(JsonNumberDouble(ObjectGet(value, "index")));
  return true;
}

bool LoadMemoryHistoryCache(MemorySnapshot& snapshot, const std::string& difficulty) {
  std::string text;
  if (!ReadTextFile(MemoryHistoryPath(), text)) return false;
  JsonValue root;
  if (!ParseJson(text, root)) return false;
  const JsonValue* events = ObjectGet(root, "events");
  if (!events || events->type != JsonValue::Type::Array) return false;
  std::vector<MemoryEvent> loaded;
  for (const auto& item : events->array) {
    MemoryEvent event;
    if (MemoryEventFromJson(item, event)) loaded.push_back(std::move(event));
  }
  if (loaded.empty()) return false;
  snapshot.events = UniqueEvents(loaded);
  RebuildMemorySnapshotJson(snapshot, difficulty);
  return true;
}

void SaveMemoryHistoryCache(const MemorySnapshot& snapshot) {
  if (snapshot.history_json.empty() || snapshot.history_json == "null") return;
  WriteTextFile(MemoryHistoryPath(), Widen(snapshot.history_json));
}

std::wstring TempComparePath() {
  wchar_t temp[MAX_PATH]{};
  DWORD length = GetTempPathW(MAX_PATH, temp);
  std::wstring dir = length ? std::wstring(temp, length) : L".\\";
  return dir + L"tbh-agent-compare.txt";
}

std::wstring ParentDir(std::wstring path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
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

std::wstring PythonSummaryPath() {
  std::wstring build_dir = ParentDir(ExePath());
  std::wstring agent_dir = ParentDir(build_dir);
  std::wstring outros_dir = ParentDir(agent_dir);
  std::wstring sibling = outros_dir + L"\\tbh-farm-local\\runtime\\save-summary.json";
  DWORD attrs = GetFileAttributesW(sibling.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return sibling;

  wchar_t profile[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  std::wstring user = length ? std::wstring(profile, length) : L"";
  return user + L"\\OneDrive\\Documentos\\Outros\\tbh-farm-local\\runtime\\save-summary.json";
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

  std::wstring build_dir = ParentDir(ExePath());
  std::wstring agent_dir = ParentDir(build_dir);
  std::wstring sibling = ParentDir(agent_dir) + L"\\tbh-farm-local\\runtime\\items.json";
  attrs = GetFileAttributesW(sibling.c_str());
  resolved = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) ? sibling : cached_json;
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

JsonValue BuildBonusStatSummary(const JsonValue& stat, const std::string& section, const std::string& type, double percent) {
  JsonValue out = JsonValue::Object();
  ObjectSet(out, "type", JsonValue::String(type));
  ObjectSet(out, "stat", CopyOrNull(ObjectGet(stat, "stat")));
  ObjectSet(out, "percent", JsonValue::Number(percent));
  ObjectSet(out, "display", CopyOrNull(ObjectGet(stat, "disp")));
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
  ObjectSet(out, "display", CopyOrNull(ObjectGet(stat, "disp")));
  return out;
}

const JsonValue* ItemByKey(const std::string& key) {
  static bool loaded = false;
  static std::map<std::string, std::string> item_json_by_key;
  static std::map<std::string, JsonValue> parsed_items;

  if (!loaded) {
    loaded = true;
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_ITEMS_INDEX), RT_RCDATA);
    HGLOBAL handle = resource ? LoadResource(nullptr, resource) : nullptr;
    const char* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
    DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
    if (data && size) {
      std::string text(data, data + size);
      size_t start = 0;
      while (start < text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
          item_json_by_key[line.substr(0, tab)] = line.substr(tab + 1);
        }
        if (end == std::string::npos) break;
        start = end + 1;
      }
    }
  }

  auto parsed = parsed_items.find(key);
  if (parsed != parsed_items.end()) return &parsed->second;
  auto raw = item_json_by_key.find(key);
  if (raw == item_json_by_key.end()) return nullptr;
  JsonValue item;
  if (!ParseJson(raw->second, item)) return nullptr;
  auto inserted = parsed_items.insert({key, std::move(item)});
  return &inserted.first->second;
}

JsonValue BuildEquippedItems(const JsonValue& hero,
                             const std::map<std::string, const JsonValue*>& item_by_uid,
                             double& exp_bonus,
                             double& gold_bonus,
                             JsonValue& bonus_sources) {
  JsonValue equipped = JsonValue::Array();
  const JsonValue* equipped_ids = ObjectGet(hero, "equippedItemIds");
  if (!equipped_ids || equipped_ids->type != JsonValue::Type::Array) return equipped;

  for (const auto& unique_id : equipped_ids->array) {
    if (IsZeroNumber(&unique_id)) continue;
    auto saved_it = item_by_uid.find(JsonNumberKey(&unique_id));
    if (saved_it == item_by_uid.end()) continue;
    const JsonValue* saved_item = saved_it->second;
    const JsonValue* item_key = ObjectGet(*saved_item, "ItemKey");
    const JsonValue* item = ItemByKey(JsonNumberKey(item_key));
    if (!item) continue;

    JsonValue all_stats = JsonValue::Array();
    JsonValue bonus_stats = JsonValue::Array();
    const JsonValue* stats = ObjectGet(*item, "stats");
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
          bonus_sources.array.push_back(BuildBonusSource(hero, item, item_key, type, percent, stat));
        }
      }
    }

    JsonValue out = JsonValue::Object();
    ObjectSet(out, "uniqueId", unique_id);
    ObjectSet(out, "itemKey", CopyOrNull(item_key));
    ObjectSet(out, "name", CopyOrNull(ObjectGet(*item, "name")));
    ObjectSet(out, "grade", CopyOrNull(ObjectGet(*item, "grade")));
    ObjectSet(out, "part", CopyOrNull(ObjectGet(*item, "parts")));
    ObjectSet(out, "icon", CopyOrNull(ObjectGet(*item, "icon")));
    ObjectSet(out, "level", CopyOrNull(ObjectGet(*item, "level")));
    ObjectSet(out, "variant", CopyOrNull(ObjectGet(*item, "variant")));
    ObjectSet(out, "stats", std::move(all_stats));
    ObjectSet(out, "bonusStats", std::move(bonus_stats));
    equipped.array.push_back(std::move(out));
  }
  return equipped;
}

JsonValue BuildHeroSummary(const JsonValue& hero, bool include_equipment,
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
    ObjectSet(out, "equippedItems", BuildEquippedItems(hero, item_by_uid, exp_bonus, gold_bonus, bonus_sources));
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

JsonValue GoldQuantity(const JsonValue* currencies) {
  const JsonValue* gold = FindByNumberKey(currencies, "Key", "100001");
  return CopyOrNull(gold ? ObjectGet(*gold, "Quantity") : nullptr);
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
        party.array.push_back(BuildHeroSummary(*it->second, true, item_by_uid, exp_bonus, gold_bonus, bonus_sources));
      }
    }
  }

  JsonValue unlocked = JsonValue::Array();
  if (heroes && heroes->type == JsonValue::Type::Array) {
    for (const auto& hero : heroes->array) {
      if (JsonBool(ObjectGet(hero, "IsUnLock"))) {
        unlocked.array.push_back(BuildHeroSummary(hero, false, item_by_uid, exp_bonus, gold_bonus, bonus_sources));
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
  ObjectSet(out, "equipmentBonuses", std::move(equipment_bonuses));
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
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam + "\",\"save\":" + save +
         ",\"clears\":" + memory.clears_json +
         ",\"history\":" + memory.history_json +
         ",\"watcher\":" + memory.watcher_json + "}";
}

std::string CachedPayload(const Config& config, const WorkerState& state) {
  std::string steam_raw = Utf8(config.steam_id);
  if (steam_raw.empty() && !state.save.steam_id.empty()) steam_raw = state.save.steam_id;
  if (steam_raw.empty()) steam_raw = "default";
  std::string steam = JsonEscape(steam_raw);
  std::string save = state.has_save ? state.save.summary_json : "null";
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam + "\",\"save\":" + save +
         ",\"clears\":" + state.memory.clears_json +
         ",\"history\":" + state.memory.history_json +
         ",\"watcher\":" + state.memory.watcher_json + "}";
}

bool CompareField(const std::string& name, const std::string& cpp_value, const std::string& py_value, std::wstring& error) {
  if (cpp_value == py_value) return true;
  error = L"Divergência em " + Widen(name) + L": C++='" + Widen(cpp_value) + L"' Python='" + Widen(py_value) + L"'";
  return false;
}

bool CompareField(const std::string& name, long long cpp_value, long long py_value, std::wstring& error) {
  if (cpp_value == py_value) return true;
  error = L"Divergência em " + Widen(name) + L": C++=" + std::to_wstring(cpp_value) + L" Python=" + std::to_wstring(py_value);
  return false;
}

bool CompareWithPythonSummary(const SaveSummary& summary, std::wstring& error) {
  std::string py;
  std::wstring path = PythonSummaryPath();
  if (!ReadTextFile(path, py)) {
    error = L"Não encontrei o save-summary do Python para comparar: " + path;
    return false;
  }

  JsonValue py_json;
  JsonValue cpp_json;
  if (!ParseJson(py, py_json)) {
    error = L"save-summary do Python não é JSON válido.";
    return false;
  }
  if (!ParseJson(summary.summary_json, cpp_json)) {
    error = L"Resumo C++ não é JSON válido.";
    return false;
  }

  std::string py_normalized = JsonSerialize(py_json);
  std::string cpp_normalized = JsonSerialize(cpp_json);
  if (py_normalized == cpp_normalized) return true;

  error = L"Resumo C++ ainda difere do Python. C++=" + std::to_wstring(cpp_normalized.size()) +
          L" bytes, Python=" + std::to_wstring(py_normalized.size()) + L" bytes.";
  return false;
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
  std::wstring compare_error;
  if (!CompareWithPythonSummary(summary, compare_error)) {
    return L"Sync bloqueado. " + compare_error;
  }
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

bool RefreshMemoryCache(WorkerState& state, bool force_discover = false) {
  DWORD now = GetTickCount();
  DWORD pid = FindProcessIdByName(L"TaskBarHero.exe");
  std::string difficulty = state.has_save ? DifficultyFromStageKey(state.save.current_stage_key) : "NORMAL";
  bool process_changed = pid != state.memory_pid;
  if (process_changed) {
    state.memory_regions.clear();
    state.memory_pid = pid;
    if (pid && LoadMemoryRegionCache(pid, state.memory_regions)) {
      state.last_region_discovery = now;
      PostStatus(L"Cache de memoria carregado (" + std::to_wstring(state.memory_regions.size()) + L" regioes).");
      if (LoadMemoryHistoryCache(state.memory, difficulty)) {
        PostStatus(L"Historico de memoria local carregado (" + std::to_wstring(state.memory.events.size()) + L" eventos).");
      }
    }
  }

  bool rediscover = force_discover || state.memory_regions.empty();
  bool due = force_discover || rediscover || now - state.last_memory_scan > 5000;
  if (!due) return false;

  DWORD scan_start = GetTickCount();
  size_t region_count_before = state.memory_regions.size();
  PostStatus(rediscover ? L"Lendo memoria com calibracao completa..." : L"Lendo memoria pelo cache...");
  MemorySnapshot previous_memory = state.memory;
  state.memory = ReadMemorySnapshot(difficulty, &state.memory_regions, rediscover, rediscover ? 0 : 24);
  if (!rediscover && state.memory.events.empty()) {
    PostStatus(L"Cache de memoria invalido. Recalibrando...");
    scan_start = GetTickCount();
    state.memory = ReadMemorySnapshot(difficulty, &state.memory_regions, true);
    rediscover = true;
  } else if (!rediscover) {
    state.memory.events = MergeMemoryEvents(previous_memory.events, state.memory.events);
    RebuildMemorySnapshotJson(state.memory, difficulty);
  }
  DWORD scan_ms = GetTickCount() - scan_start;
  state.last_memory_scan = now;
  PostStatus(L"Memoria lida em " + std::to_wstring(scan_ms) + L"ms (" +
             std::to_wstring(state.memory.events.size()) + L" eventos, " +
             std::to_wstring(state.memory_regions.size()) + L" regioes).");
  if (rediscover) {
    state.last_region_discovery = now;
    SaveMemoryRegionCache(state.memory_pid, state.memory_regions);
    PostStatus(L"Cache de memoria salvo (" + std::to_wstring(state.memory_regions.size()) + L" regioes).");
  } else if (state.memory_regions.size() != region_count_before) {
    SaveMemoryRegionCache(state.memory_pid, state.memory_regions);
    PostStatus(L"Cache de memoria compactado (" + std::to_wstring(region_count_before) + L" -> " +
               std::to_wstring(state.memory_regions.size()) + L" regioes).");
  }
  SaveMemoryHistoryCache(state.memory);
  return true;
}

std::string WatcherStatusJson(const std::string& message) {
  JsonValue watcher = JsonValue::Object();
  ObjectSet(watcher, "source", JsonValue::String("cpp-agent"));
  ObjectSet(watcher, "updatedAt", JsonValue::Number(static_cast<double>(time(nullptr))));
  ObjectSet(watcher, "message", JsonValue::String(message));
  return JsonSerialize(watcher);
}

bool SyncCachedPayload(const Config& config, WorkerState& state, bool force = false) {
  PostStatus(L"Montando payload...");
  std::string payload = CachedPayload(config, state);
  std::string hash = Fnv1aHash(payload);
  if (!force && hash == state.last_payload_hash) return false;

  if (config.server.empty() || config.token.empty()) {
    state.last_payload_hash = hash;
    PostStatus(L"Dados atualizados localmente. Configure servidor/token para sync.");
    return false;
  }

  PostStatus(L"Enviando sync (" + std::to_wstring(payload.size()) + L" bytes)...");
  std::wstring result = PostJsonPayload(config, payload);
  if (result.rfind(L"HTTP 200", 0) == 0 || result.rfind(L"HTTP 201", 0) == 0) {
    state.last_payload_hash = hash;
    PostStatus(L"Sync OK. Dados enviados.");
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
  PostStatus(L"Worker iniciado. Monitorando save e memoria.");
  RefreshSaveCache(state);
  bool game_ready = EnsureGameRunning();
  bool first_sync_done = false;

  while (WaitForSingleObject(g_worker_stop, 1000) == WAIT_TIMEOUT) {
    Config config = LoadConfig();
    bool changed = false;
    changed = RefreshSaveCache(state) || changed;

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

    if (!first_sync_done || changed) {
      first_sync_done = true;
      SyncCachedPayload(config, state, true);
    }

    changed = RefreshMemoryCache(state) || changed;
    if (changed) {
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

      g_autostart = AddCheckbox(hwnd, IDC_AUTOSTART, L"Iniciar com Windows", config.auto_start, 144, 210, 220, 24, font);

      AddButton(hwnd, IDC_OPEN, L"Abrir UI", 144, 250, 120, 34, font);

      g_status = CreateWindowW(L"STATIC", L"Pronto.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               24, 310, 550, 70, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
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
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wparam);
      int notification = HIWORD(wparam);
      if ((id == IDC_SERVER || id == IDC_TOKEN || id == IDC_STEAM) && notification == EN_CHANGE) {
        SaveConfigFromUi();
      } else if (id == IDC_OPEN) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        OpenWebUi(config);
      } else if (id == IDC_AUTOSTART) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        SetStatus(config.auto_start ? L"Inicialização com Windows ativada." : L"Inicialização com Windows desativada.");
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
      return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_DESTROY:
      StopWorker();
      if (font) DeleteObject(font);
      if (title_font) DeleteObject(title_font);
      g_main_window = nullptr;
      PostQuitMessage(0);
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
          std::wstring error;
          result = CompareWithPythonSummary(summary, error) ? L"OK: resumo C++ completo bate com Python" : L"FAIL: " + error;
        }
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

  INITCOMMONCONTROLSEX controls{};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&controls);

  const wchar_t* class_name = L"TBHCompanionAgentWindow";
  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = instance;
  wc.lpszClassName = class_name;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
  wc.hbrBackground = CreateSolidBrush(RGB(23, 26, 29));
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0, class_name, L"TBH Companion",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 620, 430,
                              nullptr, nullptr, instance, nullptr);
  if (!hwnd) return 1;

  HICON small_icon = reinterpret_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
  HICON big_icon = reinterpret_cast<HICON>(
      LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
  if (small_icon) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
  if (big_icon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));

  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
