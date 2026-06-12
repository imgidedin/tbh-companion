#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <string>
#include <vector>

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

  size_t account_pos = json.find("\"AccountSaveData\"");
  summary.owner_steam_id = LeadingDigits(ExtractJsonString(json, "ownerSteamId", account_pos == std::string::npos ? 0 : account_pos));
  summary.player_id = ExtractJsonString(json, "playerId", account_pos == std::string::npos ? 0 : account_pos);
  summary.steam_id = summary.owner_steam_id.empty() ? summary.player_id : summary.owner_steam_id;

  size_t common_pos = json.find("\"commonSaveData\"");
  summary.version = ExtractJsonString(json, "version", common_pos == std::string::npos ? 0 : common_pos);
  summary.current_stage_key = ExtractJsonInt(json, "currentStageKey", common_pos == std::string::npos ? 0 : common_pos);
  summary.current_stage_wave = ExtractJsonInt(json, "currentStageWave", common_pos == std::string::npos ? 0 : common_pos);
  summary.max_completed_stage = ExtractJsonInt(json, "maxCompletedStage", common_pos == std::string::npos ? 0 : common_pos);
  summary.arranged_pet_key = ExtractJsonInt(json, "ArrangedPetKey", common_pos == std::string::npos ? 0 : common_pos);

  std::string currencies = ExtractArraySection(json, "currenySaveDatas");
  size_t gold_pos = currencies.find("\"Key\":100001");
  if (gold_pos == std::string::npos) gold_pos = currencies.find("\\\"Key\\\":100001");
  summary.gold = ExtractJsonInt(currencies, "Quantity", gold_pos == std::string::npos ? 0 : gold_pos);
  summary.pets_json = BuildPetsSummaryJson(ExtractArraySection(json, "PetSaveData"));
  summary.runes_json = BuildRunesSummaryJson(ExtractArraySection(json, "RuneSaveData"));
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
    save = "{\"steamId\":\"" + JsonEscape(summary.steam_id) + "\",\"ownerSteamId\":\"" + JsonEscape(summary.owner_steam_id) +
           "\",\"playerId\":\"" + JsonEscape(summary.player_id) + "\",\"version\":\"" + JsonEscape(summary.version) +
           "\",\"currentStageKey\":" + std::to_string(summary.current_stage_key) +
           ",\"currentStageWave\":" + std::to_string(summary.current_stage_wave) +
           ",\"maxCompletedStage\":" + std::to_string(summary.max_completed_stage) +
           ",\"arrangedPetKey\":" + std::to_string(summary.arranged_pet_key) +
           ",\"gold\":" + std::to_string(summary.gold) +
           ",\"pets\":" + summary.pets_json +
           ",\"runes\":" + summary.runes_json +
           ",\"agent\":\"cpp-save-mvp\"}";
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

  if (!CompareField("steamId", summary.steam_id, ExtractJsonString(py, "steamId"), error)) return false;
  if (!CompareField("ownerSteamId", summary.owner_steam_id, ExtractJsonString(py, "ownerSteamId"), error)) return false;
  if (!CompareField("playerId", summary.player_id, ExtractJsonString(py, "playerId"), error)) return false;
  if (!CompareField("version", summary.version, ExtractJsonString(py, "version"), error)) return false;
  if (!CompareField("currentStageKey", summary.current_stage_key, ExtractJsonInt(py, "currentStageKey"), error)) return false;
  if (!CompareField("currentStageWave", summary.current_stage_wave, ExtractJsonInt(py, "currentStageWave"), error)) return false;
  if (!CompareField("maxCompletedStage", summary.max_completed_stage, ExtractJsonInt(py, "maxCompletedStage"), error)) return false;
  if (!CompareField("arrangedPetKey", summary.arranged_pet_key, ExtractJsonInt(py, "arrangedPetKey"), error)) return false;
  if (!CompareField("gold", summary.gold, ExtractLastJsonInt(py, "gold"), error)) return false;

  int cpp_pets = CountKey(summary.pets_json, "petKey");
  int py_pets = CountKey(py, "petKey");
  if (!CompareField("pets.count", cpp_pets, py_pets, error)) return false;

  int cpp_runes = CountKey(summary.runes_json, "runeKey");
  int py_runes = CountKey(py, "runeKey");
  if (!CompareField("runes.count", cpp_runes, py_runes, error)) return false;

  return true;
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
  return L"Comparação parcial OK. Envio bloqueado até o resumo C++ ficar idêntico ao Python. Faltam heróis, equipamentos, atributos e bônus.";

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
          result = CompareWithPythonSummary(summary, error) ? L"OK: resumo C++ bate com Python nos campos portados" : L"FAIL: " + error;
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
  wc.hbrBackground = CreateSolidBrush(RGB(23, 26, 29));
  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0, class_name, L"TBH Companion",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 620, 400,
                              nullptr, nullptr, instance, nullptr);
  if (!hwnd) return 1;

  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
