#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr int IDC_SERVER = 1001;
constexpr int IDC_TOKEN = 1002;
constexpr int IDC_STEAM = 1003;
constexpr int IDC_SAVE = 1004;
constexpr int IDC_TEST = 1005;
constexpr int IDC_OPEN = 1006;
constexpr int IDC_STATUS = 1007;
constexpr int IDC_READ_SAVE = 1008;

constexpr wchar_t DEFAULT_SAVE_RELATIVE[] = L"\\AppData\\LocalLow\\TesseractStudio\\TaskbarHero\\SaveFile_Live.es3";
constexpr char DEFAULT_ES3_KEY[] = "emuMqG3bLYJ938ZDCfieWJ";

HINSTANCE g_instance = nullptr;
HWND g_server = nullptr;
HWND g_token = nullptr;
HWND g_steam = nullptr;
HWND g_status = nullptr;

struct Config {
  std::wstring server = L"https://tbh.gided.in";
  std::wstring token;
  std::wstring steam_id;
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

std::wstring ConfigPath() {
  wchar_t local_app_data[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  std::wstring dir = length ? std::wstring(local_app_data, length) : L".";
  dir += L"\\TBH Companion";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\config.ini";
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

  return config;
}

void SaveConfig(const Config& config) {
  std::wstring path = ConfigPath();
  WritePrivateProfileStringW(L"server", L"url", config.server.c_str(), path.c_str());
  WritePrivateProfileStringW(L"server", L"token", config.token.c_str(), path.c_str());
  WritePrivateProfileStringW(L"player", L"steam_id", config.steam_id.c_str(), path.c_str());
}

Config ReadConfigFromUi() {
  Config config;
  config.server = GetWindowTextString(g_server);
  config.token = GetWindowTextString(g_token);
  config.steam_id = GetWindowTextString(g_steam);
  return config;
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
  std::wstring build_dir = ParentDir(ExePath());
  std::wstring agent_dir = ParentDir(build_dir);
  std::wstring candidate = agent_dir + L"\\runtime\\items.json";
  DWORD attrs = GetFileAttributesW(candidate.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return candidate;

  std::wstring outros_dir = ParentDir(agent_dir);
  std::wstring sibling = outros_dir + L"\\tbh-farm-local\\runtime\\items.json";
  attrs = GetFileAttributesW(sibling.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return sibling;

  wchar_t profile[MAX_PATH]{};
  DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  std::wstring user = length ? std::wstring(profile, length) : L"";
  return user + L"\\OneDrive\\Documentos\\Outros\\tbh-farm-local\\runtime\\items.json";
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

JsonValue BuildEquippedItems(const JsonValue& hero,
                             const std::map<std::string, const JsonValue*>& item_by_uid,
                             const std::map<std::string, const JsonValue*>& items_by_key,
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
    auto item_it = items_by_key.find(JsonNumberKey(item_key));
    if (item_it == items_by_key.end()) continue;
    const JsonValue* item = item_it->second;

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
                           const std::map<std::string, const JsonValue*>& items_by_key,
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
    ObjectSet(out, "equippedItems", BuildEquippedItems(hero, item_by_uid, items_by_key, exp_bonus, gold_bonus, bonus_sources));
  }
  return out;
}

bool LoadItemsByKey(std::map<std::string, const JsonValue*>& items_by_key, JsonValue& items_root) {
  std::string text;
  if (!ReadTextFile(ItemsJsonPath(), text)) return false;
  if (!ParseJson(text, items_root) || items_root.type != JsonValue::Type::Array) return false;
  items_by_key = IndexArrayByNumberKey(&items_root, "key");
  return true;
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

  JsonValue items_root;
  std::map<std::string, const JsonValue*> items_by_key;
  LoadItemsByKey(items_by_key, items_root);
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
        party.array.push_back(BuildHeroSummary(*it->second, true, item_by_uid, items_by_key, exp_bonus, gold_bonus, bonus_sources));
      }
    }
  }

  JsonValue unlocked = JsonValue::Array();
  if (heroes && heroes->type == JsonValue::Type::Array) {
    for (const auto& hero : heroes->array) {
      if (JsonBool(ObjectGet(hero, "IsUnLock"))) {
        unlocked.array.push_back(BuildHeroSummary(hero, false, item_by_uid, items_by_key, exp_bonus, gold_bonus, bonus_sources));
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
  std::string steam_raw = config.steam_id.empty() ? (summary.steam_id.empty() ? "default" : summary.steam_id) : Utf8(config.steam_id);
  std::string steam = JsonEscape(steam_raw);
  std::string save = "null";
  if (has_save) {
    save = summary.summary_json;
  }
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam + "\",\"save\":" + save +
         ",\"clears\":null,\"history\":null,\"watcher\":{\"agent\":\"cpp-save-mvp\"}}";
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

std::wstring PostIngest(const Config& config) {
  if (config.server.empty()) return L"Configure a URL do servidor.";
  if (config.token.empty()) return L"Configure o token de ingest.";

  SaveSummary summary;
  if (!ReadSaveSummary(summary)) return L"Não foi possível ler o save C++.";
  std::wstring compare_error;
  if (!CompareWithPythonSummary(summary, compare_error)) {
    return L"Sync bloqueado. " + compare_error;
  }

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

  std::string payload = SavePayload(config);
  std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + config.token + L"\r\n";

  BOOL ok = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
                               payload.data(), static_cast<DWORD>(payload.size()),
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

      AddButton(hwnd, IDC_SAVE, L"Salvar", 144, 220, 100, 34, font);
      AddButton(hwnd, IDC_TEST, L"Comparar", 256, 220, 120, 34, font);
      AddButton(hwnd, IDC_OPEN, L"Abrir UI", 388, 220, 100, 34, font);
      AddButton(hwnd, IDC_READ_SAVE, L"Ler save", 500, 220, 86, 34, font);

      g_status = CreateWindowW(L"STATIC", L"Pronto.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               24, 278, 550, 70, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
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
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(wparam);
      if (id == IDC_SAVE) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        SetStatus(L"Configuração salva.");
      } else if (id == IDC_OPEN) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        OpenWebUi(config);
      } else if (id == IDC_TEST) {
        Config config = ReadConfigFromUi();
        SaveConfig(config);
        SetStatus(L"Comparando C++ com Python...");
        std::wstring result = PostIngest(config);
        SetStatus(result);
      } else if (id == IDC_READ_SAVE) {
        SetStatus(L"Lendo save...");
        std::wstring steam_id = ReadSteamIdFromSave();
        if (steam_id.empty()) {
          SetStatus(L"Não foi possível ler o SteamID do save padrão.");
        } else {
          SetWindowTextW(g_steam, steam_id.c_str());
          Config config = ReadConfigFromUi();
          SaveConfig(config);
          SetStatus(L"SteamID lido do save: " + steam_id);
        }
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
      if (font) DeleteObject(font);
      if (title_font) DeleteObject(title_font);
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 620, 400,
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
