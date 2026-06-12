#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

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

std::string TestPayload(const Config& config) {
  std::string steam = JsonEscape(Utf8(config.steam_id.empty() ? L"default" : config.steam_id));
  return "{\"steamId\":\"" + steam + "\",\"siteId\":\"" + steam +
         "\",\"save\":{\"steamId\":\"" + steam + "\",\"agent\":\"cpp-mvp\"},\"clears\":null,\"history\":null,\"watcher\":{\"agent\":\"cpp-mvp\"}}";
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

  std::string payload = TestPayload(config);
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
      AddButton(hwnd, IDC_TEST, L"Testar sync", 256, 220, 120, 34, font);
      AddButton(hwnd, IDC_OPEN, L"Abrir UI", 388, 220, 100, 34, font);

      g_status = CreateWindowW(L"STATIC", L"Pronto.", WS_CHILD | WS_VISIBLE | SS_LEFT,
                               24, 278, 550, 70, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
      SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
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
        SetStatus(L"Enviando teste...");
        std::wstring result = PostIngest(config);
        SetStatus(result);
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
