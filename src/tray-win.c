#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TRAY_CALLBACK_MSG (WM_APP + 1)
#define TRAY_OPEN_ID 1001
#define TRAY_QUIT_ID 1002
#define TRAY_TIMER_ID 1
#define APP_ICON_ID 1
#define POPUP_ITEM_HEIGHT 26
#define POPUP_WIDTH 132
#define POPUP_PADDING 4

static NOTIFYICONDATAW g_tray_icon = {0};
static DWORD g_parent_pid = 0;
static HANDLE g_single_instance_mutex = NULL;
static HWND g_popup_hwnd = NULL;
static int g_popup_hovered_item = 0;
static HFONT g_popup_font = NULL;
static const GUID g_tray_guid = {0x7b111f26, 0x43a5, 0x4d7d, {0xa2, 0xa1, 0x10, 0xee, 0x14, 0x76, 0xac, 0x50}};
static const char *g_control_window_class = "SmashedPumpkinControlWindow";
static const char *g_popup_window_class = "SmashedPumpkinTrayPopup";

static void destroy_popup(void);

static bool
send_app_command(BOOL show_command)
{
  HWND hwnd = FindWindowA(g_control_window_class, NULL);
  if (hwnd == NULL) {
    return false;
  }

  UINT msg = RegisterWindowMessageW(show_command
                                      ? L"Rotstein.SmashedPumpkin.Tray.Show"
                                      : L"Rotstein.SmashedPumpkin.Tray.Quit");
  if (msg == 0) {
    return false;
  }

  return PostMessageA(hwnd, msg, 0, 0) != 0;
}

static bool
path_exists(const char *path)
{
  if (path == NULL || *path == '\0') {
    return false;
  }
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void
resolve_sibling_binary(const char *name, char *out, size_t out_len)
{
  if (out == NULL || out_len == 0) {
    return;
  }
  out[0] = '\0';

  char exe_path[MAX_PATH] = {0};
  DWORD written = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
  if (written == 0 || written >= MAX_PATH) {
    return;
  }

  char *slash = strrchr(exe_path, '\\');
  if (slash == NULL) {
    slash = strrchr(exe_path, '/');
  }
  if (slash == NULL) {
    return;
  }
  slash[1] = '\0';
  _snprintf(out, out_len, "%s%s", exe_path, name);
  out[out_len - 1] = '\0';
}

static void
launch_main(const char *arg)
{
  if (arg != NULL) {
    if (strcmp(arg, "--show") == 0 && send_app_command(TRUE)) {
      return;
    }
    if (strcmp(arg, "--quit") == 0 && send_app_command(FALSE)) {
      return;
    }
  }

  char binary[MAX_PATH] = {0};
  resolve_sibling_binary("smashed-pumpkin.exe", binary, sizeof(binary));
  const char *target = path_exists(binary) ? binary : "smashed-pumpkin.exe";
  char working_dir[MAX_PATH] = {0};
  if (path_exists(binary)) {
    _snprintf(working_dir, sizeof(working_dir), "%s", binary);
    working_dir[sizeof(working_dir) - 1] = '\0';
    char *slash = strrchr(working_dir, '\\');
    if (slash == NULL) {
      slash = strrchr(working_dir, '/');
    }
    if (slash != NULL) {
      *slash = '\0';
    }
  }

  STARTUPINFOA si = {0};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOWNORMAL;

  PROCESS_INFORMATION pi = {0};
  char command_line[2048] = {0};
  if (arg != NULL && *arg != '\0') {
    _snprintf(command_line, sizeof(command_line), "\"%s\" %s", target, arg);
  } else {
    _snprintf(command_line, sizeof(command_line), "\"%s\"", target);
  }
  command_line[sizeof(command_line) - 1] = '\0';

  if (CreateProcessA(target,
                     command_line,
                     NULL,
                     NULL,
                     FALSE,
                     CREATE_NO_WINDOW,
                     NULL,
                     working_dir[0] != '\0' ? working_dir : NULL,
                     &si,
                     &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return;
  }

  ShellExecuteA(NULL, "open", target, arg, working_dir[0] != '\0' ? working_dir : NULL, SW_SHOWNORMAL);
}

static HICON
load_tray_icon(void)
{
  HINSTANCE instance = GetModuleHandleW(NULL);
  HICON icon = (HICON)LoadImageW(instance,
                                 MAKEINTRESOURCEW(APP_ICON_ID),
                                 IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON),
                                 LR_DEFAULTCOLOR);
  if (icon != NULL) {
    return icon;
  }

  return LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
}

static bool
parent_process_alive(void)
{
  if (g_parent_pid == 0) {
    return true;
  }
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, g_parent_pid);
  if (process == NULL) {
    return false;
  }
  DWORD wait = WaitForSingleObject(process, 0);
  CloseHandle(process);
  return wait == WAIT_TIMEOUT;
}

static COLORREF
popup_background_color(void)
{
  return RGB(46, 48, 53);
}

static COLORREF
popup_border_color(void)
{
  return RGB(74, 77, 84);
}

static COLORREF
popup_text_color(void)
{
  return RGB(244, 246, 248);
}

static COLORREF
popup_hover_color(void)
{
  return RGB(64, 67, 74);
}

static int
popup_window_height(void)
{
  return (POPUP_PADDING * 2) + (POPUP_ITEM_HEIGHT * 2);
}

static RECT
popup_item_rect(int command_id)
{
  RECT rect = {
    POPUP_PADDING,
    POPUP_PADDING,
    POPUP_WIDTH - POPUP_PADDING,
    POPUP_PADDING + POPUP_ITEM_HEIGHT
  };

  if (command_id == TRAY_QUIT_ID) {
    rect.top += POPUP_ITEM_HEIGHT;
    rect.bottom += POPUP_ITEM_HEIGHT;
  }

  return rect;
}

static int
popup_hit_test(int x, int y)
{
  RECT open_rect = popup_item_rect(TRAY_OPEN_ID);
  RECT quit_rect = popup_item_rect(TRAY_QUIT_ID);

  POINT pt = { x, y };
  if (PtInRect(&open_rect, pt)) {
    return TRAY_OPEN_ID;
  }
  if (PtInRect(&quit_rect, pt)) {
    return TRAY_QUIT_ID;
  }
  return 0;
}

static void
execute_popup_command(int command_id)
{
  if (command_id == TRAY_OPEN_ID) {
    launch_main("--show");
  } else if (command_id == TRAY_QUIT_ID) {
    send_app_command(FALSE);
    PostQuitMessage(0);
  }
}

static LRESULT CALLBACK
popup_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
  switch (msg) {
  case WM_MOUSEMOVE:
  {
    TRACKMOUSEEVENT track = {0};
    int hovered = popup_hit_test(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
    if (hovered != g_popup_hovered_item) {
      g_popup_hovered_item = hovered;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd;
    TrackMouseEvent(&track);
    return 0;
  }
  case WM_MOUSELEAVE:
    if (g_popup_hovered_item != 0) {
      g_popup_hovered_item = 0;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
  {
    int command_id = popup_hit_test(GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
    destroy_popup();
    if (command_id != 0) {
      execute_popup_command(command_id);
    }
    return 0;
  }
  case WM_KEYDOWN:
    if (w_param == VK_ESCAPE) {
      destroy_popup();
      return 0;
    }
    break;
  case WM_ACTIVATE:
    if (LOWORD(w_param) == WA_INACTIVE) {
      destroy_popup();
    }
    return 0;
  case WM_PAINT:
  {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT client;
    HBRUSH bg_brush = CreateSolidBrush(popup_background_color());
    HBRUSH hover_brush = CreateSolidBrush(popup_hover_color());
    HPEN border_pen = CreatePen(PS_SOLID, 1, popup_border_color());
    HPEN old_pen;
    HGDIOBJ old_brush;
    HGDIOBJ old_font;
    SetBkMode(hdc, TRANSPARENT);

    GetClientRect(hwnd, &client);
    old_pen = (HPEN)SelectObject(hdc, border_pen);
    old_brush = SelectObject(hdc, bg_brush);
    RoundRect(hdc, client.left, client.top, client.right, client.bottom, 14, 14);

    RECT open_rect = popup_item_rect(TRAY_OPEN_ID);
    RECT quit_rect = popup_item_rect(TRAY_QUIT_ID);
    if (g_popup_hovered_item == TRAY_OPEN_ID) {
      FillRect(hdc, &open_rect, hover_brush);
    }
    if (g_popup_hovered_item == TRAY_QUIT_ID) {
      FillRect(hdc, &quit_rect, hover_brush);
    }

    if (g_popup_font != NULL) {
      old_font = SelectObject(hdc, g_popup_font);
    } else {
      old_font = NULL;
    }
    SetTextColor(hdc, popup_text_color());
    DrawTextA(hdc, "Open", -1, &open_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    DrawTextA(hdc, "Quit", -1, &quit_rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    if (old_font != NULL) {
      SelectObject(hdc, old_font);
    }
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(border_pen);
    DeleteObject(bg_brush);
    DeleteObject(hover_brush);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_DESTROY:
    if (g_popup_hwnd == hwnd) {
      g_popup_hwnd = NULL;
      g_popup_hovered_item = 0;
    }
    return 0;
  default:
    break;
  }
  return DefWindowProcA(hwnd, msg, w_param, l_param);
}

static void
destroy_popup(void)
{
  if (g_popup_hwnd != NULL) {
    HWND popup = g_popup_hwnd;
    g_popup_hwnd = NULL;
    DestroyWindow(popup);
  }
}

static void
show_context_menu(HWND hwnd, int x, int y)
{
  destroy_popup();

  POINT cursor = {0};
  (void)x;
  (void)y;
  GetCursorPos(&cursor);

  RECT work_area = {0};
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info = {0};
  monitor_info.cbSize = sizeof(monitor_info);
  if (GetMonitorInfoW(monitor, &monitor_info)) {
    work_area = monitor_info.rcWork;
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  }

  int width = POPUP_WIDTH;
  int height = popup_window_height();
  int popup_x = cursor.x;
  int popup_y = cursor.y;

  if (popup_x < work_area.left + 8) {
    popup_x = work_area.left + 8;
  }
  if (popup_x + width > work_area.right - 8) {
    popup_x = work_area.right - width - 8;
  }
  if (popup_y + height > work_area.bottom - 8) {
    popup_y = cursor.y - height;
  }
  if (popup_y < work_area.top + 8) {
    popup_y = work_area.top + 8;
  }

  g_popup_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                                 g_popup_window_class,
                                 "Smashed Pumpkin Tray Menu",
                                 WS_POPUP,
                                 popup_x,
                                 popup_y,
                                 width,
                                 height,
                                 hwnd,
                                 NULL,
                                 GetModuleHandleA(NULL),
                                 NULL);
  if (g_popup_hwnd == NULL) {
    return;
  }

  ShowWindow(g_popup_hwnd, SW_SHOWNORMAL);
  UpdateWindow(g_popup_hwnd);
  SetForegroundWindow(g_popup_hwnd);
}

static LRESULT CALLBACK
tray_window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
  switch (msg) {
  case WM_CREATE:
    SetTimer(hwnd, TRAY_TIMER_ID, 5000, NULL);
    return 0;
  case WM_TIMER:
    if (w_param == TRAY_TIMER_ID && !parent_process_alive()) {
      PostQuitMessage(0);
    }
    return 0;
  case WM_COMMAND:
    if (LOWORD(w_param) == TRAY_OPEN_ID) {
      launch_main("--show");
      return 0;
    }
    if (LOWORD(w_param) == TRAY_QUIT_ID) {
      send_app_command(FALSE);
      PostQuitMessage(0);
      return 0;
    }
    break;
  case TRAY_CALLBACK_MSG:
  {
    UINT event = LOWORD((DWORD)l_param);
    if (event == 0) {
      event = (UINT)l_param;
    }

    if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
      show_context_menu(hwnd, GET_X_LPARAM(w_param), GET_Y_LPARAM(w_param));
      return 0;
    }
    if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
      destroy_popup();
      launch_main("--show");
      return 0;
    }
    break;
  }
  case WM_DESTROY:
    destroy_popup();
    if (g_popup_font != NULL) {
      DeleteObject(g_popup_font);
      g_popup_font = NULL;
    }
    if (g_tray_icon.hIcon != NULL) {
      DestroyIcon(g_tray_icon.hIcon);
      g_tray_icon.hIcon = NULL;
    }
    if (g_single_instance_mutex != NULL) {
      CloseHandle(g_single_instance_mutex);
      g_single_instance_mutex = NULL;
    }
    Shell_NotifyIconW(NIM_DELETE, &g_tray_icon);
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcA(hwnd, msg, w_param, l_param);
}

int
main(int argc, char **argv)
{
  g_single_instance_mutex = CreateMutexW(NULL, FALSE, L"Local\\Rotstein.SmashedPumpkin.Tray");
  if (g_single_instance_mutex == NULL) {
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(g_single_instance_mutex);
    g_single_instance_mutex = NULL;
    return 0;
  }

  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--parent-pid=", 13) == 0) {
      g_parent_pid = (DWORD)strtoul(argv[i] + 13, NULL, 10);
    }
  }

  HINSTANCE instance = GetModuleHandleA(NULL);
  const char *class_name = "SmashedPumpkinTrayWindow";
  wchar_t tip[] = L"Smashed Pumpkin";

  LOGFONTA log_font = {0};
  log_font.lfHeight = -14;
  log_font.lfWeight = FW_MEDIUM;
  strncpy(log_font.lfFaceName, "Segoe UI", LF_FACESIZE - 1);
  g_popup_font = CreateFontIndirectA(&log_font);

  WNDCLASSEXA window_class = {0};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = tray_window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;
  if (RegisterClassExA(&window_class) == 0) {
    return 1;
  }

  WNDCLASSEXA popup_class = {0};
  popup_class.cbSize = sizeof(popup_class);
  popup_class.lpfnWndProc = popup_window_proc;
  popup_class.hInstance = instance;
  popup_class.hCursor = LoadCursor(NULL, IDC_ARROW);
  popup_class.lpszClassName = g_popup_window_class;
  RegisterClassExA(&popup_class);

  HWND hwnd = CreateWindowExA(0,
                              class_name,
                              "Smashed Pumpkin Tray",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              NULL,
                              NULL,
                              instance,
                              NULL);
  if (hwnd == NULL) {
    return 1;
  }

  ZeroMemory(&g_tray_icon, sizeof(g_tray_icon));
  g_tray_icon.cbSize = sizeof(g_tray_icon);
  g_tray_icon.hWnd = hwnd;
  g_tray_icon.uID = 1;
  g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
  g_tray_icon.uCallbackMessage = TRAY_CALLBACK_MSG;
  g_tray_icon.guidItem = g_tray_guid;
  g_tray_icon.hIcon = load_tray_icon();
  wcsncpy(g_tray_icon.szTip, tip, ARRAYSIZE(g_tray_icon.szTip) - 1);
  if (!Shell_NotifyIconW(NIM_ADD, &g_tray_icon)) {
    DestroyWindow(hwnd);
    return 1;
  }
  g_tray_icon.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &g_tray_icon);

  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  return 0;
}
