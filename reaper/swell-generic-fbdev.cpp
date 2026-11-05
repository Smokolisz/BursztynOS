#ifndef SWELL_PROVIDED_BY_APP

#include "swell.h"
#include "swell-internal.h"
#include "swell-dlggen.h"
#include "../wdlcstring.h"
#include "../lice/lice.h"
#include "../assocarray.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

void SWELL_internalLICEpaint(HWND hwnd, LICE_IBitmap *bmout, int bmout_xpos, int bmout_ypos, bool forceref);
LRESULT SWELL_SendMouseMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int x_root, int y_root);

namespace {

constexpr int kTitleBarHeight = 22;
constexpr int kBorderWidth = 1;
constexpr int kResizeGripSize = 12;
constexpr int kCloseButtonWidth = 22;
constexpr int kMinimumWindowSize = 80;
constexpr DWORD kDoubleClickMilliseconds = 400;
constexpr int kDoubleClickDistance = 4;
constexpr int kKeyStateCount = KEY_MAX + 1;

const LICE_pixel kDesktopColor = LICE_RGBA(0x20, 0x20, 0x20, 0xFF);
const LICE_pixel kActiveTitleColor = LICE_RGBA(0xFF, 0xBF, 0x00, 0xFF);
const LICE_pixel kInactiveTitleColor = LICE_RGBA(0x8A, 0x5A, 0x00, 0xFF);
const LICE_pixel kActiveTitleTextColor = LICE_RGBA(0x1A, 0x0B, 0x00, 0xFF);
const LICE_pixel kInactiveTitleTextColor = LICE_RGBA(0xD1, 0xD5, 0xDB, 0xFF);
const LICE_pixel kBorderColor = LICE_RGBA(0x10, 0x10, 0x10, 0xFF);
const LICE_pixel kCloseColor = LICE_RGBA(0xAA, 0x00, 0x00, 0xFF);
const LICE_pixel kCloseTextColor = LICE_RGBA(0xFF, 0xFF, 0xFF, 0xFF);

struct Screen {
  int fb_fd = -1;
  int input_fd = -1;
  int width = 0;
  int height = 0;
  int stride = 0;
  size_t mapped_size = 0;
  LICE_pixel *pixels = nullptr;
  LICE_WrapperBitmap *bitmap = nullptr;
};

struct DirtyRegion {
  RECT rect;
  bool empty = true;

  void add(const RECT &r)
  {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (empty)
    {
      rect = r;
      empty = false;
      return;
    }
    rect.left = wdl_min(rect.left, r.left);
    rect.top = wdl_min(rect.top, r.top);
    rect.right = wdl_max(rect.right, r.right);
    rect.bottom = wdl_max(rect.bottom, r.bottom);
  }
};

enum class DragMode { kNone, kMove, kResize };

struct PointerState {
  int x = 0;
  int y = 0;
  bool left = false;
  bool right = false;
  bool middle = false;
  DragMode drag_mode = DragMode::kNone;
  HWND drag_window = nullptr;
  int drag_origin_x = 0;
  int drag_origin_y = 0;
  RECT drag_origin_rect = {};
  DWORD last_click_time[3] = {};
  int last_click_x[3] = {};
  int last_click_y[3] = {};
};

struct KeyboardState {
  bool down[kKeyStateCount] = {};

  bool any(int a, int b) const { return down[a] || down[b]; }
  bool shift() const { return any(KEY_LEFTSHIFT, KEY_RIGHTSHIFT); }
  bool control() const { return any(KEY_LEFTCTRL, KEY_RIGHTCTRL); }
  bool alt() const { return any(KEY_LEFTALT, KEY_RIGHTALT); }
  bool meta() const { return any(KEY_LEFTMETA, KEY_RIGHTMETA); }
};

Screen g_screen;
PointerState g_pointer;
KeyboardState g_keyboard;
WDL_PtrList<HWND__> g_stack;
WDL_IntKeyedArray<DirtyRegion> *g_window_dirty;
DirtyRegion g_screen_dirty;
HWND g_main_window;
DWORD g_last_message_pos;
bool g_in_paint;

bool open_screen()
{
  if (g_screen.pixels) return true;
  const int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
  if (fb < 0) return false;

  fb_var_screeninfo variable = {};
  fb_fix_screeninfo fixed = {};
  if (ioctl(fb, FBIOGET_VSCREENINFO, &variable) < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fixed) < 0 || variable.bits_per_pixel != 32)
  {
    close(fb);
    return false;
  }

  const size_t size = fixed.line_length * (size_t)variable.yres;
  void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
  if (mapping == MAP_FAILED)
  {
    close(fb);
    return false;
  }

  g_screen.fb_fd = fb;
  g_screen.input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  g_screen.width = (int)variable.xres;
  g_screen.height = (int)variable.yres;
  g_screen.stride = (int)fixed.line_length;
  g_screen.mapped_size = size;
  g_screen.pixels = (LICE_pixel *)mapping;
  g_screen.bitmap = new LICE_WrapperBitmap(g_screen.pixels, g_screen.width, g_screen.height, g_screen.stride / 4, false);
  g_window_dirty = new WDL_IntKeyedArray<DirtyRegion>;
  return true;
}

bool has_caption(HWND hwnd) { return (hwnd->m_style & WS_CAPTION) && hwnd != g_main_window; }

RECT frame_rect(HWND hwnd)
{
  RECT r = hwnd->m_position;
  if (has_caption(hwnd))
  {
    r.left -= kBorderWidth;
    r.right += kBorderWidth;
    r.bottom += kBorderWidth;
    r.top -= kTitleBarHeight;
  }
  return r;
}

bool intersect(const RECT &a, const RECT &b, RECT *out)
{
  out->left = wdl_max(a.left, b.left);
  out->top = wdl_max(a.top, b.top);
  out->right = wdl_min(a.right, b.right);
  out->bottom = wdl_min(a.bottom, b.bottom);
  return out->right > out->left && out->bottom > out->top;
}

RECT screen_rect()
{
  RECT r = {0, 0, g_screen.width, g_screen.height};
  return r;
}

void invalidate_screen(const RECT &r)
{
  RECT clipped;
  if (intersect(r, screen_rect(), &clipped)) g_screen_dirty.add(clipped);
}

bool is_active(HWND hwnd)
{
  return hwnd->m_oswindow == SWELL_focused_oswindow;
}

void draw_frame(HWND hwnd, const RECT &clip)
{
  if (!has_caption(hwnd)) return;
  const RECT frame = frame_rect(hwnd);
  RECT visible;
  if (!intersect(frame, clip, &visible)) return;

  LICE_SubBitmap target(g_screen.bitmap, visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top);
  const int origin_x = frame.left - visible.left;
  const int origin_y = frame.top - visible.top;
  const int width = frame.right - frame.left;
  const int height = frame.bottom - frame.top;
  const bool active = is_active(hwnd);

  LICE_FillRect(&target, origin_x, origin_y, width, height, kBorderColor, 1.0f, LICE_BLIT_MODE_COPY);
  LICE_FillRect(&target, origin_x + kBorderWidth, origin_y + kBorderWidth, width - 2 * kBorderWidth, kTitleBarHeight - kBorderWidth,
                active ? kActiveTitleColor : kInactiveTitleColor, 1.0f, LICE_BLIT_MODE_COPY);
  LICE_DrawText(&target, origin_x + 6, origin_y + 7, hwnd->m_title.Get(), active ? kActiveTitleTextColor : kInactiveTitleTextColor, 1.0f, LICE_BLIT_MODE_COPY);
  LICE_FillRect(&target, origin_x + width - kCloseButtonWidth - kBorderWidth, origin_y + kBorderWidth, kCloseButtonWidth, kTitleBarHeight - kBorderWidth,
                kCloseColor, 1.0f, LICE_BLIT_MODE_COPY);
  LICE_DrawText(&target, origin_x + width - kCloseButtonWidth + 6, origin_y + 7, "X", kCloseTextColor, 1.0f, LICE_BLIT_MODE_COPY);
}

void composite(const RECT &clip)
{
  LICE_FillRect(g_screen.bitmap, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, kDesktopColor, 1.0f, LICE_BLIT_MODE_COPY);
  for (int i = 0; i < g_stack.GetSize(); ++i)
  {
    HWND hwnd = g_stack.Get(i);
    draw_frame(hwnd, clip);
    if (!hwnd->m_backingstore) continue;
    RECT visible;
    if (!intersect(hwnd->m_position, clip, &visible)) continue;
    LICE_Blit(g_screen.bitmap, hwnd->m_backingstore, visible.left, visible.top,
              visible.left - hwnd->m_position.left, visible.top - hwnd->m_position.top,
              visible.right - visible.left, visible.bottom - visible.top, 1.0f, LICE_BLIT_MODE_COPY);
  }
}

void present_screen()
{
  if (g_screen_dirty.empty) return;
  const RECT r = g_screen_dirty.rect;
  g_screen_dirty.empty = true;
  composite(r);
  const size_t offset = (size_t)r.top * g_screen.stride;
  const size_t length = (size_t)(r.bottom - r.top) * g_screen.stride;
  const uintptr_t start = ((uintptr_t)g_screen.pixels + offset) & ~(uintptr_t)4095;
  msync((void *)start, ((uintptr_t)g_screen.pixels + offset + length) - start, MS_SYNC);
}

void paint_window(HWND hwnd)
{
  DirtyRegion *dirty = g_window_dirty->GetPtr((INT_PTR)hwnd);
  if (!dirty || dirty->empty || !hwnd->m_oswindow) return;

  const int width = hwnd->m_position.right - hwnd->m_position.left;
  const int height = hwnd->m_position.bottom - hwnd->m_position.top;
  if (width <= 0 || height <= 0)
  {
    dirty->empty = true;
    return;
  }

  if (!hwnd->m_backingstore) hwnd->m_backingstore = new LICE_MemBitmap;
  const bool resized = hwnd->m_backingstore->resize(width, height);

  RECT client = {0, 0, width, height};
  RECT region = client;
  if (!resized) intersect(dirty->rect, client, &region);
  dirty->empty = true;

  if (region.right > region.left && region.bottom > region.top)
  {
    LICE_SubBitmap target(hwnd->m_backingstore, region.left, region.top, region.right - region.left, region.bottom - region.top);
    g_in_paint = true;
    SWELL_internalLICEpaint(hwnd, &target, region.left, region.top, resized);
    g_in_paint = false;
    RECT on_screen = region;
    OffsetRect(&on_screen, hwnd->m_position.left, hwnd->m_position.top);
    invalidate_screen(on_screen);
  }
}

void paint_all_windows()
{
  for (int i = 0; i < g_stack.GetSize(); ++i)
  {
    paint_window(g_stack.Get(i));
  }
}

int stack_index(HWND hwnd) { return g_stack.Find(hwnd); }

void raise_window(HWND hwnd)
{
  const int index = stack_index(hwnd);
  if (index < 0) return;
  g_stack.Delete(index);
  g_stack.Add(hwnd);
  invalidate_screen(frame_rect(hwnd));

  const int count = g_stack.GetSize();
  int position = 0;
  for (int visited = 0; visited < count; ++visited)
  {
    HWND candidate = g_stack.Get(position);
    if (candidate != hwnd && (candidate->m_owner == hwnd || candidate->m_israised))
    {
      g_stack.Delete(position);
      g_stack.Add(candidate);
      invalidate_screen(frame_rect(candidate));
    }
    else
    {
      ++position;
    }
  }
}

HWND window_at(int x, int y)
{
  for (int i = g_stack.GetSize() - 1; i >= 0; --i)
  {
    HWND hwnd = g_stack.Get(i);
    const RECT r = frame_rect(hwnd);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return hwnd;
  }
  return nullptr;
}

bool in_title_bar(HWND hwnd, int x, int y)
{
  if (!has_caption(hwnd)) return false;
  const RECT frame = frame_rect(hwnd);
  return y >= frame.top && y < hwnd->m_position.top && x >= frame.left && x < frame.right;
}

bool in_close_button(HWND hwnd, int x, int y)
{
  const RECT frame = frame_rect(hwnd);
  return in_title_bar(hwnd, x, y) && x >= frame.right - kCloseButtonWidth - kBorderWidth;
}

bool in_resize_grip(HWND hwnd, int x, int y)
{
  if (!has_caption(hwnd) || !(hwnd->m_style & WS_THICKFRAME)) return false;
  const RECT frame = frame_rect(hwnd);
  return x >= frame.right - kResizeGripSize && y >= frame.bottom - kResizeGripSize && x < frame.right && y < frame.bottom;
}

void set_window_rect(HWND hwnd, const RECT &r)
{
  const RECT old_frame = frame_rect(hwnd);
  const bool moved = r.left != hwnd->m_position.left || r.top != hwnd->m_position.top;
  const bool resized = (r.right - r.left) != (hwnd->m_position.right - hwnd->m_position.left) ||
                       (r.bottom - r.top) != (hwnd->m_position.bottom - hwnd->m_position.top);
  hwnd->m_position = r;
  invalidate_screen(old_frame);
  invalidate_screen(frame_rect(hwnd));
  if (moved) SendMessage(hwnd, WM_MOVE, 0, 0);
  if (resized) SendMessage(hwnd, WM_SIZE, hwnd->m_is_maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, 0);
}

HWND mouse_target(HWND top, const POINT &local)
{
  HWND captured = GetCapture();
  if (captured) return captured;
  if (!top || swell_window_wants_all_input() == top) return top;
  return ChildWindowFromPoint(top, local);
}

void remember_pointer(int x, int y)
{
  g_pointer.x = x;
  g_pointer.y = y;
  g_last_message_pos = MAKELONG(x & 0xFFFF, y & 0xFFFF);
}

void send_mouse(HWND target, UINT message, WPARAM wparam, int x, int y, bool screen_coordinates)
{
  if (!target) return;
  POINT p = {x, y};
  if (!screen_coordinates) ScreenToClient(target, &p);
  target->Retain();
  SWELL_SendMouseMessage(target, message, wparam, MAKELPARAM(p.x, p.y), x, y);
  target->Release();
}

void handle_pointer_motion()
{
  const int x = g_pointer.x;
  const int y = g_pointer.y;
  if (g_pointer.drag_mode != DragMode::kNone && g_pointer.drag_window)
  {
    HWND hwnd = g_pointer.drag_window;
    const int dx = x - g_pointer.drag_origin_x;
    const int dy = y - g_pointer.drag_origin_y;
    RECT r = g_pointer.drag_origin_rect;
    if (g_pointer.drag_mode == DragMode::kMove)
    {
      OffsetRect(&r, dx, dy);
      if (r.top < kTitleBarHeight) OffsetRect(&r, 0, kTitleBarHeight - r.top);
    }
    else
    {
      r.right = wdl_max(r.left + kMinimumWindowSize, r.right + dx);
      r.bottom = wdl_max(r.top + kMinimumWindowSize, r.bottom + dy);
    }
    set_window_rect(hwnd, r);
    return;
  }

  HWND top = window_at(x, y);
  POINT local = {x, y};
  if (top)
  {
    local.x -= top->m_position.left;
    local.y -= top->m_position.top;
  }
  send_mouse(mouse_target(top, local), WM_MOUSEMOVE, 0, x, y, false);
}

bool begin_window_management_drag(HWND top, int x, int y)
{
  if (GetCapture() || !top || !IsWindowEnabled(top)) return false;
  if (in_close_button(top, x, y))
  {
    if (!DestroyPopupMenus() && !SendMessage(top, WM_CLOSE, 0, 0)) SendMessage(top, WM_COMMAND, IDCANCEL, 0);
    return true;
  }
  const bool title = in_title_bar(top, x, y);
  const bool grip = in_resize_grip(top, x, y);
  if (!title && !grip) return false;
  g_pointer.drag_mode = title ? DragMode::kMove : DragMode::kResize;
  g_pointer.drag_window = top;
  g_pointer.drag_origin_x = x;
  g_pointer.drag_origin_y = y;
  g_pointer.drag_origin_rect = top->m_position;
  return true;
}

void focus_top_window(HWND top)
{
  if (!top || !IsWindowEnabled(top)) return;
  const bool is_menu = !(top->m_style & WS_CAPTION) && top != g_main_window;
  if (!is_menu)
  {
    raise_window(top);
    if (SWELL_focused_oswindow != top->m_oswindow)
    {
      HWND previous = (HWND)SWELL_focused_oswindow;
      SWELL_focused_oswindow = top->m_oswindow;
      if (previous) invalidate_screen(frame_rect(previous));
      invalidate_screen(frame_rect(top));
      swell_on_toplevel_raise(top->m_oswindow);
    }
  }
}

bool is_double_click(int button, int x, int y)
{
  const DWORD now = GetTickCount();
  const bool result = now - g_pointer.last_click_time[button] < kDoubleClickMilliseconds &&
                      wdl_abs(x - g_pointer.last_click_x[button]) <= kDoubleClickDistance &&
                      wdl_abs(y - g_pointer.last_click_y[button]) <= kDoubleClickDistance;
  g_pointer.last_click_time[button] = result ? 0 : now;
  g_pointer.last_click_x[button] = x;
  g_pointer.last_click_y[button] = y;
  return result;
}

void handle_button(int button, bool pressed)
{
  static const UINT kDownMessages[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
  static const UINT kUpMessages[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
  static const UINT kDoubleMessages[3] = {WM_LBUTTONDBLCLK, WM_RBUTTONDBLCLK, WM_MBUTTONDBLCLK};

  const int x = g_pointer.x;
  const int y = g_pointer.y;

  if (!pressed && g_pointer.drag_mode != DragMode::kNone)
  {
    g_pointer.drag_mode = DragMode::kNone;
    g_pointer.drag_window = nullptr;
    return;
  }

  HWND top = window_at(x, y);

  if (pressed)
  {
    swell_dlg_destroyspare();
    focus_top_window(top);
    if (button == 0 && begin_window_management_drag(top, x, y)) return;
  }

  POINT local = {x, y};
  if (top)
  {
    local.x -= top->m_position.left;
    local.y -= top->m_position.top;
  }
  HWND target = mouse_target(top, local);
  if (!target) return;

  if (pressed)
  {
    target->Retain();
    if (IsWindowEnabled(target)) SendMessage(target, WM_MOUSEACTIVATE, 0, 0);
    target->Release();
    const UINT message = is_double_click(button, x, y) ? kDoubleMessages[button] : kDownMessages[button];
    send_mouse(target, message, 0, x, y, false);
  }
  else
  {
    send_mouse(target, kUpMessages[button], 0, x, y, false);
  }
}

void handle_wheel(int delta)
{
  const int x = g_pointer.x;
  const int y = g_pointer.y;
  HWND top = window_at(x, y);
  POINT local = {x, y};
  if (top)
  {
    local.x -= top->m_position.left;
    local.y -= top->m_position.top;
  }
  HWND target = mouse_target(top, local);
  if (!target) return;
  target->Retain();
  SWELL_SendMouseMessage(target, WM_MOUSEWHEEL, (WPARAM)((delta * 120) << 16), MAKELPARAM(x, y), x, y);
  target->Release();
}

struct KeyMapping {
  UINT virtual_key;
  bool is_virtual;
  char plain;
  char shifted;
  bool extended;
};

KeyMapping map_key(int code)
{
  static const char kPlainRow[] = "\0\0" "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";
  static const char kShiftRow[] = "\0\0" "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

  switch (code)
  {
    case KEY_ESC: return {VK_ESCAPE, true, 0, 0, false};
    case KEY_BACKSPACE: return {VK_BACK, true, 0, 0, false};
    case KEY_TAB: return {VK_TAB, true, 0, 0, false};
    case KEY_ENTER: return {VK_RETURN, true, 0, 0, false};
    case KEY_KPENTER: return {VK_RETURN, true, 0, 0, true};
    case KEY_SPACE: return {VK_SPACE, true, 0, 0, false};
    case KEY_F1: return {VK_F1, true, 0, 0, false};
    case KEY_F2: return {VK_F2, true, 0, 0, false};
    case KEY_F3: return {VK_F3, true, 0, 0, false};
    case KEY_F4: return {VK_F4, true, 0, 0, false};
    case KEY_F5: return {VK_F5, true, 0, 0, false};
    case KEY_F6: return {VK_F6, true, 0, 0, false};
    case KEY_F7: return {VK_F7, true, 0, 0, false};
    case KEY_F8: return {VK_F8, true, 0, 0, false};
    case KEY_F9: return {VK_F9, true, 0, 0, false};
    case KEY_F10: return {VK_F10, true, 0, 0, false};
    case KEY_F11: return {VK_F11, true, 0, 0, false};
    case KEY_F12: return {VK_F12, true, 0, 0, false};
    case KEY_HOME: return {VK_HOME, true, 0, 0, true};
    case KEY_END: return {VK_END, true, 0, 0, true};
    case KEY_UP: return {VK_UP, true, 0, 0, true};
    case KEY_DOWN: return {VK_DOWN, true, 0, 0, true};
    case KEY_LEFT: return {VK_LEFT, true, 0, 0, true};
    case KEY_RIGHT: return {VK_RIGHT, true, 0, 0, true};
    case KEY_PAGEUP: return {VK_PRIOR, true, 0, 0, true};
    case KEY_PAGEDOWN: return {VK_NEXT, true, 0, 0, true};
    case KEY_INSERT: return {VK_INSERT, true, 0, 0, true};
    case KEY_DELETE: return {VK_DELETE, true, 0, 0, true};
    case KEY_KP0: return {VK_NUMPAD0, true, 0, 0, false};
    case KEY_KP1: return {VK_NUMPAD1, true, 0, 0, false};
    case KEY_KP2: return {VK_NUMPAD2, true, 0, 0, false};
    case KEY_KP3: return {VK_NUMPAD3, true, 0, 0, false};
    case KEY_KP4: return {VK_NUMPAD4, true, 0, 0, false};
    case KEY_KP5: return {VK_NUMPAD5, true, 0, 0, false};
    case KEY_KP6: return {VK_NUMPAD6, true, 0, 0, false};
    case KEY_KP7: return {VK_NUMPAD7, true, 0, 0, false};
    case KEY_KP8: return {VK_NUMPAD8, true, 0, 0, false};
    case KEY_KP9: return {VK_NUMPAD9, true, 0, 0, false};
    case KEY_KPASTERISK: return {VK_MULTIPLY, true, 0, 0, false};
    case KEY_KPPLUS: return {VK_ADD, true, 0, 0, false};
    case KEY_KPMINUS: return {VK_SUBTRACT, true, 0, 0, false};
    case KEY_KPDOT: return {VK_DECIMAL, true, 0, 0, false};
    case KEY_KPSLASH: return {VK_DIVIDE, true, 0, 0, true};
    default: break;
  }
  if (code >= 0 && code < (int)sizeof(kPlainRow) && kPlainRow[code])
  {
    return {0, false, kPlainRow[code], kShiftRow[code], false};
  }
  return {0, false, 0, 0, false};
}

UINT modifier_virtual_key(int code)
{
  switch (code)
  {
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT: return VK_SHIFT;
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL: return VK_CONTROL;
    case KEY_LEFTALT:
    case KEY_RIGHTALT: return VK_MENU;
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA: return VK_LWIN;
    default: return 0;
  }
}

void dispatch_key(HWND window, UINT message, UINT key, int modifiers)
{
  HWND focus = GetFocusIncludeMenus();
  HWND target = window;
  if (focus && window && IsChild(window, focus)) target = focus;
  else if (focus && focus->m_oswindow && !(focus->m_style & WS_CAPTION)) target = focus;
  if (!target) return;

  MSG msg = {target, message, key, modifiers};
  INT_PTR extra_flags = 0;
  if (DialogBoxIsActive()) extra_flags |= 1;
  if (SWELLAppMain(SWELLAPP_PROCESSMESSAGE, (INT_PTR)&msg, extra_flags) <= 0)
  {
    SendMessage(target, message, key, modifiers);
  }
}

void handle_key(int code, bool pressed)
{
  if (code < 0 || code >= kKeyStateCount) return;
  g_keyboard.down[code] = pressed;

  HWND window = (HWND)SWELL_focused_oswindow;
  if (!window) window = g_main_window;
  if (!window) return;

  swell_dlg_destroyspare();

  int modifiers = 0;
  if (g_keyboard.control()) modifiers |= FCONTROL;
  if (g_keyboard.alt()) modifiers |= FALT;
  if (g_keyboard.meta()) modifiers |= FLWIN;
  if (g_keyboard.shift()) modifiers |= FSHIFT;

  const UINT message = pressed ? WM_KEYDOWN : WM_KEYUP;

  if (const UINT modifier = modifier_virtual_key(code))
  {
    dispatch_key(window, pressed ? WM_SYSKEYDOWN : WM_SYSKEYUP, modifier, modifiers | FVIRTKEY);
    return;
  }

  const KeyMapping mapping = map_key(code);
  if (mapping.is_virtual)
  {
    int flags = modifiers;
    if (mapping.virtual_key != VK_SPACE) flags |= FVIRTKEY;
    if (mapping.extended) flags |= 1 << 24;
    dispatch_key(window, message, mapping.virtual_key, flags);
    return;
  }
  if (!mapping.plain) return;

  const char plain = mapping.plain;
  if (plain >= 'a' && plain <= 'z')
  {
    dispatch_key(window, message, (UINT)(plain - 'a' + 'A'), modifiers | FVIRTKEY);
  }
  else if (plain >= '0' && plain <= '9' && !g_keyboard.shift())
  {
    dispatch_key(window, message, (UINT)plain, modifiers | FVIRTKEY);
  }
  else
  {
    const char character = g_keyboard.shift() ? mapping.shifted : mapping.plain;
    dispatch_key(window, message, (UINT)(unsigned char)character, modifiers & ~FSHIFT);
  }
}

void request_main_window_close()
{
  if (!g_main_window || !IsWindowEnabled(g_main_window) || DestroyPopupMenus()) return;
  if (!SendMessage(g_main_window, WM_CLOSE, 0, 0)) SendMessage(g_main_window, WM_COMMAND, IDCANCEL, 0);
}

void process_input()
{
  if (g_screen.input_fd < 0) return;
  input_event events[64];
  bool moved = false;
  for (;;)
  {
    const ssize_t bytes = read(g_screen.input_fd, events, sizeof(events));
    if (bytes <= 0) break;
    const int count = (int)(bytes / sizeof(input_event));
    for (int i = 0; i < count; ++i)
    {
      const input_event &event = events[i];
      if (event.type == EV_ABS)
      {
        if (event.code == ABS_X) remember_pointer(event.value, g_pointer.y);
        else if (event.code == ABS_Y) remember_pointer(g_pointer.x, event.value);
        moved = true;
      }
      else if (event.type == EV_REL && event.code == REL_WHEEL)
      {
        if (moved) handle_pointer_motion();
        moved = false;
        handle_wheel(event.value);
      }
      else if (event.type == EV_KEY)
      {
        if (moved) handle_pointer_motion();
        moved = false;
        if (event.code == BTN_LEFT) { g_pointer.left = event.value; handle_button(0, event.value); }
        else if (event.code == BTN_RIGHT) { g_pointer.right = event.value; handle_button(1, event.value); }
        else if (event.code == BTN_MIDDLE) { g_pointer.middle = event.value; handle_button(2, event.value); }
        else if (event.code == KEY_CLOSE) { if (event.value) request_main_window_close(); }
        else handle_key(event.code, event.value != 0);
      }
    }
  }
  if (moved) handle_pointer_motion();
}

void remove_from_stack(HWND hwnd)
{
  const int index = stack_index(hwnd);
  if (index >= 0)
  {
    invalidate_screen(frame_rect(hwnd));
    g_stack.Delete(index);
  }
  if (g_window_dirty) g_window_dirty->Delete((INT_PTR)hwnd);
  if (g_main_window == hwnd) g_main_window = nullptr;
  if (g_pointer.drag_window == hwnd)
  {
    g_pointer.drag_window = nullptr;
    g_pointer.drag_mode = DragMode::kNone;
  }
}

void place_new_window(HWND hwnd)
{
  const bool becomes_main = !g_main_window && !hwnd->m_owner && (hwnd->m_style & WS_THICKFRAME) && (hwnd->m_style & WS_CAPTION);
  if (becomes_main)
  {
    g_main_window = hwnd;
    RECT full = screen_rect();
    hwnd->m_position = full;
    hwnd->m_is_maximized = true;
    return;
  }
  if (!(hwnd->m_style & WS_CAPTION)) return;
  RECT r = hwnd->m_position;
  const int width = r.right - r.left;
  const int height = r.bottom - r.top;
  if (r.top < kTitleBarHeight) OffsetRect(&r, 0, kTitleBarHeight - r.top);
  if (r.right > g_screen.width) OffsetRect(&r, wdl_max(0, g_screen.width - width) - r.left, 0);
  if (r.bottom > g_screen.height) OffsetRect(&r, 0, wdl_max(kTitleBarHeight, g_screen.height - height) - r.top);
  hwnd->m_position = r;
}

}

void swell_oswindow_destroy(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow)
  {
    if (SWELL_focused_oswindow == hwnd->m_oswindow) SWELL_focused_oswindow = nullptr;
    remove_from_stack(hwnd);
    hwnd->m_oswindow = nullptr;
    delete hwnd->m_backingstore;
    hwnd->m_backingstore = nullptr;
    if (!SWELL_focused_oswindow && g_stack.GetSize())
    {
      SWELL_focused_oswindow = g_stack.Get(g_stack.GetSize() - 1)->m_oswindow;
    }
  }
}

void swell_oswindow_update_text(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow && has_caption(hwnd))
  {
    RECT r = frame_rect(hwnd);
    r.bottom = hwnd->m_position.top;
    invalidate_screen(r);
  }
}

void swell_oswindow_maximize(HWND hwnd, bool wantmax)
{
  if (!hwnd || !hwnd->m_oswindow || hwnd == g_main_window) return;
  hwnd->m_is_maximized = wantmax;
  if (wantmax)
  {
    RECT r = {kBorderWidth, kTitleBarHeight, g_screen.width - kBorderWidth, g_screen.height - kBorderWidth};
    set_window_rect(hwnd, r);
  }
}

void swell_oswindow_focus(HWND hwnd)
{
  if (!hwnd)
  {
    SWELL_focused_oswindow = nullptr;
    return;
  }
  while (hwnd && !hwnd->m_oswindow) hwnd = hwnd->m_parent;
  if (!hwnd || hwnd->m_oswindow == SWELL_focused_oswindow) return;
  HWND previous = (HWND)SWELL_focused_oswindow;
  SWELL_focused_oswindow = hwnd->m_oswindow;
  raise_window(hwnd);
  if (previous) invalidate_screen(frame_rect(previous));
  invalidate_screen(frame_rect(hwnd));
}

void swell_recalcMinMaxInfo(HWND hwnd) {}

void SWELL_initargs(int *argc, char ***argv) { open_screen(); }

void swell_oswindow_updatetoscreen(HWND hwnd, RECT *rect)
{
  if (!hwnd || !hwnd->m_oswindow || !rect) return;
  RECT on_screen = *rect;
  OffsetRect(&on_screen, hwnd->m_position.left, hwnd->m_position.top);
  invalidate_screen(on_screen);
  present_screen();
}

void swell_oswindow_manage(HWND hwnd, bool wantfocus)
{
  if (!hwnd) return;
  const bool is_visible = hwnd->m_oswindow != nullptr;
  const bool want_visible = !hwnd->m_parent && hwnd->m_visible;
  if (is_visible == want_visible) return;
  if (!want_visible)
  {
    swell_oswindow_destroy(hwnd);
    return;
  }
  if (!open_screen()) return;

  place_new_window(hwnd);
  hwnd->m_oswindow = hwnd;
  hwnd->m_backingstore = new LICE_MemBitmap;
  g_stack.Add(hwnd);
  const RECT client = {0, 0, hwnd->m_position.right - hwnd->m_position.left, hwnd->m_position.bottom - hwnd->m_position.top};
  swell_oswindow_invalidate(hwnd, &client);
  invalidate_screen(frame_rect(hwnd));
  if (wantfocus || !SWELL_focused_oswindow)
  {
    SWELL_focused_oswindow = hwnd->m_oswindow;
  }
  if (hwnd == g_main_window) SendMessage(hwnd, WM_SIZE, SIZE_MAXIMIZED, 0);
}

void SWELL_RunEvents()
{
  if (!g_screen.pixels || g_in_paint) return;
  process_input();
  paint_all_windows();
  present_screen();
}

void swell_oswindow_update_style(HWND hwnd, LONG oldstyle)
{
  if (hwnd && hwnd->m_oswindow) invalidate_screen(frame_rect(hwnd));
}

void swell_oswindow_update_enable(HWND hwnd) {}

int SWELL_SetWindowLevel(HWND hwnd, int newlevel)
{
  int rv = 0;
  if (hwnd)
  {
    rv = hwnd->m_israised ? 1 : 0;
    hwnd->m_israised = newlevel > 0;
  }
  return rv;
}

void SWELL_GetViewPort(RECT *r, const RECT *sourcerect, bool wantWork)
{
  open_screen();
  r->left = r->top = 0;
  r->right = g_screen.width ? g_screen.width : 1024;
  r->bottom = g_screen.height ? g_screen.height : 768;
}

bool GetWindowRect(HWND hwnd, RECT *r)
{
  if (!hwnd) return false;
  if (hwnd->m_oswindow)
  {
    *r = hwnd->m_position;
    return true;
  }
  r->left = r->top = 0;
  ClientToScreen(hwnd, (LPPOINT)r);
  r->right = r->left + hwnd->m_position.right - hwnd->m_position.left;
  r->bottom = r->top + hwnd->m_position.bottom - hwnd->m_position.top;
  return true;
}

void swell_oswindow_begin_resize(SWELL_OSWINDOW wnd) {}

void swell_oswindow_resize(SWELL_OSWINDOW wnd, int reposflag, RECT f)
{
  HWND hwnd = (HWND)wnd;
  if (!hwnd) return;
  invalidate_screen(frame_rect(hwnd));
  invalidate_screen(f);
  const RECT client = {0, 0, f.right - f.left, f.bottom - f.top};
  swell_oswindow_invalidate(hwnd, &client);
}

void swell_oswindow_postresize(HWND hwnd, RECT f)
{
  if (hwnd && hwnd->m_oswindow) invalidate_screen(frame_rect(hwnd));
}

void swell_oswindow_invalidate(HWND hwnd, const RECT *r)
{
  if (!hwnd || !g_window_dirty) return;
  DirtyRegion *dirty = g_window_dirty->GetPtr((INT_PTR)hwnd);
  if (!dirty)
  {
    g_window_dirty->Insert((INT_PTR)hwnd, DirtyRegion());
    dirty = g_window_dirty->GetPtr((INT_PTR)hwnd);
  }
  if (r)
  {
    dirty->add(*r);
  }
  else
  {
    const RECT client = {0, 0, hwnd->m_position.right - hwnd->m_position.left, hwnd->m_position.bottom - hwnd->m_position.top};
    dirty->add(client);
  }
}

void UpdateWindow(HWND hwnd)
{
  while (hwnd && !hwnd->m_oswindow) hwnd = hwnd->m_parent;
  if (!hwnd || g_in_paint) return;
  paint_window(hwnd);
  present_screen();
}

static WDL_IntKeyedArray<HANDLE> g_clipboard_data(GlobalFree);
static WDL_PtrList<char> g_clipboard_formats;

bool OpenClipboard(HWND hwndDlg)
{
  RegisterClipboardFormat(nullptr);
  return true;
}

void CloseClipboard() {}

UINT EnumClipboardFormats(UINT lastfmt)
{
  int x = 0;
  for (;;)
  {
    int fmt = 0;
    if (!g_clipboard_data.Enumerate(x++, &fmt)) return 0;
    if (lastfmt == 0) return fmt;
    if ((UINT)fmt == lastfmt) return g_clipboard_data.Enumerate(x++, &fmt) ? fmt : 0;
  }
}

HANDLE GetClipboardData(UINT type) { return g_clipboard_data.Get(type); }

void EmptyClipboard() { g_clipboard_data.DeleteAll(); }

void SetClipboardData(UINT type, HANDLE h)
{
  if (h) g_clipboard_data.Insert(type, h);
  else g_clipboard_data.Delete(type);
}

UINT RegisterClipboardFormat(const char *desc)
{
  if (!g_clipboard_formats.GetSize())
  {
    g_clipboard_formats.Add(strdup("SWELL__CF_TEXT"));
    g_clipboard_formats.Add(strdup("SWELL__CF_HDROP"));
  }
  if (!desc || !*desc) return 0;
  const int count = g_clipboard_formats.GetSize();
  for (int x = 0; x < count; ++x)
  {
    if (!strcmp(g_clipboard_formats.Get(x), desc)) return x + 1;
  }
  g_clipboard_formats.Add(strdup(desc));
  return count + 1;
}

void GetCursorPos(POINT *pt)
{
  pt->x = g_pointer.x;
  pt->y = g_pointer.y;
}

WORD GetAsyncKeyState(int key)
{
  bool down = false;
  switch (key)
  {
    case VK_LBUTTON: down = g_pointer.left; break;
    case VK_RBUTTON: down = g_pointer.right; break;
    case VK_MBUTTON: down = g_pointer.middle; break;
    case VK_CONTROL: down = g_keyboard.control(); break;
    case VK_MENU: down = g_keyboard.alt(); break;
    case VK_SHIFT: down = g_keyboard.shift(); break;
    case VK_LWIN: down = g_keyboard.meta(); break;
    default: break;
  }
  return down ? 0x8000 : 0;
}

DWORD GetMessagePos() { return g_last_message_pos; }

HWND SWELL_CreateXBridgeWindow(HWND viewpar, void **wref, const RECT *r)
{
  *wref = nullptr;
  return nullptr;
}

void SWELL_InitiateDragDrop(HWND hwnd, RECT *srcrect, const char *srcfn, void (*callback)(const char *dropfn)) {}
void SWELL_InitiateDragDropOfFileList(HWND hwnd, RECT *srcrect, const char **srclist, int srccount, HICON icon) {}
void SWELL_FinishDragDrop() {}

static HCURSOR g_cursor;
static int g_cursor_visible_count;

void SWELL_SetCursor(HCURSOR curs) { g_cursor = curs; }
HCURSOR SWELL_GetCursor() { return g_cursor; }
HCURSOR SWELL_GetLastSetCursor() { return g_cursor; }
bool SWELL_IsCursorVisible() { return g_cursor_visible_count >= 0; }

int SWELL_ShowCursor(BOOL bShow)
{
  g_cursor_visible_count += bShow ? 1 : -1;
  return g_cursor_visible_count;
}

BOOL SWELL_SetCursorPos(int X, int Y) { return false; }
HCURSOR SWELL_LoadCursorFromFile(const char *fn) { return nullptr; }
HCURSOR SWELL_LoadCursor(const char *_idx) { return nullptr; }

static SWELL_CursorResourceIndex *g_cursor_resources;

void SWELL_Register_Cursor_Resource(const char *idx, const char *name, int hotspot_x, int hotspot_y)
{
  auto *entry = (SWELL_CursorResourceIndex *)malloc(sizeof(SWELL_CursorResourceIndex));
  entry->hotspot.x = hotspot_x;
  entry->hotspot.y = hotspot_y;
  entry->resname = name;
  entry->cachedCursor = nullptr;
  entry->resid = idx;
  entry->_next = g_cursor_resources;
  g_cursor_resources = entry;
}

int SWELL_KeyToASCII(int wParam, int lParam, int *newflags) { return 0; }
int swell_is_app_inactive() { return 0; }
void swell_scaling_init(bool no_auto_hidpi) {}
void *SWELL_GetOSWindow(HWND hwnd, const char *type) { return nullptr; }
void *SWELL_GetOSEvent(const char *type) { return nullptr; }
void SWELL_SetViewGL(HWND h, char wantGL) {}
bool SWELL_GetViewGL(HWND h) { return false; }
bool SWELL_SetGLContextToView(HWND h) { return false; }

#endif
