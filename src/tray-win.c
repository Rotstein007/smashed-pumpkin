#include <windows.h>
#include <shellapi.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRAY_CALLBACK_MSG (WM_APP + 1)
#define TRAY_OPEN_ID 1001
#define TRAY_QUIT_ID 1002
#define TRAY_TIMER_ID 1

static NOTIFYICONDATAA g_tray_icon = {0};
static DWORD g_parent_pid = 0;

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
  char binary[MAX_PATH] = {0};
  resolve_sibling_binary("smashed-pumpkin.exe", binary, sizeof(binary));
  const char *target = path_exists(binary) ? binary : "smashed-pumpkin.exe";
  ShellExecuteA(NULL, "open", target, arg, NULL, SW_SHOWNORMAL);
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

static void
show_context_menu(HWND hwnd)
{
  POINT cursor;
  GetCursorPos(&cursor);

  HMENU menu = CreatePopupMenu();
  if (menu == NULL) {
    return;
  }
  AppendMenuA(menu, MF_STRING, TRAY_OPEN_ID, "Open");
  AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuA(menu, MF_STRING, TRAY_QUIT_ID, "Quit");

  SetForegroundWindow(hwnd);
  UINT selected = TrackPopupMenu(menu,
                                 TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                 cursor.x,
                                 cursor.y,
                                 0,
                                 hwnd,
                                 NULL);
  DestroyMenu(menu);

  if (selected == TRAY_OPEN_ID) {
    launch_main("--show");
  } else if (selected == TRAY_QUIT_ID) {
    launch_main("--quit");
    PostQuitMessage(0);
  }
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
      launch_main("--quit");
      PostQuitMessage(0);
      return 0;
    }
    break;
  case TRAY_CALLBACK_MSG:
    if (l_param == WM_RBUTTONUP || l_param == WM_LBUTTONUP || l_param == WM_CONTEXTMENU) {
      show_context_menu(hwnd);
      return 0;
    }
    break;
  case WM_DESTROY:
    Shell_NotifyIconA(NIM_DELETE, &g_tray_icon);
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
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--parent-pid=", 13) == 0) {
      g_parent_pid = (DWORD)strtoul(argv[i] + 13, NULL, 10);
    }
  }

  HINSTANCE instance = GetModuleHandleA(NULL);
  const char *class_name = "SmashedPumpkinTrayWindow";

  WNDCLASSEXA window_class = {0};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = tray_window_proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = class_name;
  if (RegisterClassExA(&window_class) == 0) {
    return 1;
  }

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
  g_tray_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  g_tray_icon.uCallbackMessage = TRAY_CALLBACK_MSG;
  g_tray_icon.hIcon = LoadIconA(NULL, IDI_APPLICATION);
  strncpy(g_tray_icon.szTip, "Smashed Pumpkin", sizeof(g_tray_icon.szTip) - 1);
  if (!Shell_NotifyIconA(NIM_ADD, &g_tray_icon)) {
    DestroyWindow(hwnd);
    return 1;
  }

  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  return 0;
}
