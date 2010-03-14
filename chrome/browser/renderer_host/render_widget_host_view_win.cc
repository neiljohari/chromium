// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/renderer_host/render_widget_host_view_win.h"

#include "app/gfx/canvas.h"
#include "app/gfx/gdi_util.h"
#include "app/l10n_util.h"
#include "app/l10n_util_win.h"
#include "app/resource_bundle.h"
#include "base/command_line.h"
#include "base/histogram.h"
#include "base/process_util.h"
#include "base/thread.h"
#include "base/win_util.h"
#include "chrome/browser/browser_accessibility_manager.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_trial.h"
#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/plugin_process_host.h"
#include "chrome/browser/renderer_host/backing_store.h"
#include "chrome/browser/renderer_host/backing_store_win.h"
#include "chrome/browser/renderer_host/gpu_view_host.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/browser/renderer_host/render_widget_host.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/native_web_keyboard_event.h"
#include "chrome/common/plugin_messages.h"
#include "chrome/common/render_messages.h"
#include "gfx/rect.h"
#include "grit/webkit_resources.h"
#include "skia/ext/skia_utils_win.h"
#include "third_party/WebKit/WebKit/chromium/public/WebInputEvent.h"
#include "third_party/WebKit/WebKit/chromium/public/win/WebInputEventFactory.h"
#include "views/accessibility/view_accessibility.h"
#include "views/focus/focus_manager.h"
#include "views/focus/focus_util_win.h"
// Included for views::kReflectedMessage - TODO(beng): move this to win_util.h!
#include "views/widget/widget_win.h"
#include "webkit/glue/plugins/plugin_constants_win.h"
#include "webkit/glue/plugins/webplugin_delegate_impl.h"
#include "webkit/glue/webcursor.h"

using base::TimeDelta;
using base::TimeTicks;
using WebKit::WebInputEvent;
using WebKit::WebInputEventFactory;
using WebKit::WebMouseEvent;
using WebKit::WebTextDirection;
using webkit_glue::WebPluginGeometry;

namespace {

// Tooltips will wrap after this width. Yes, wrap. Imagine that!
const int kTooltipMaxWidthPixels = 300;

// Maximum number of characters we allow in a tooltip.
const int kMaxTooltipLength = 1024;

const wchar_t* kRenderWidgetHostViewKey = L"__RENDER_WIDGET_HOST_VIEW__";

// A callback function for EnumThreadWindows to enumerate and dismiss
// any owned popop windows
BOOL CALLBACK DismissOwnedPopups(HWND window, LPARAM arg) {
  const HWND toplevel_hwnd = reinterpret_cast<HWND>(arg);

  if (::IsWindowVisible(window)) {
    const HWND owner = ::GetWindow(window, GW_OWNER);
    if (toplevel_hwnd == owner) {
      ::PostMessage(window, WM_CANCELMODE, 0, 0);
    }
  }

  return TRUE;
}

// Enumerates the installed keyboard layouts in this system and returns true
// if an RTL keyboard layout is installed.
// TODO(hbono): to be moved to "src/chrome/common/l10n_util.cc"?
static bool IsRTLKeyboardLayoutInstalled() {
  static enum {
    RTL_KEYBOARD_LAYOUT_NOT_INITIALIZED,
    RTL_KEYBOARD_LAYOUT_INSTALLED,
    RTL_KEYBOARD_LAYOUT_NOT_INSTALLED,
    RTL_KEYBOARD_LAYOUT_ERROR,
  } layout = RTL_KEYBOARD_LAYOUT_NOT_INITIALIZED;

  // Cache the result value.
  if (layout != RTL_KEYBOARD_LAYOUT_NOT_INITIALIZED)
    return layout == RTL_KEYBOARD_LAYOUT_INSTALLED;

  // Retrieve the number of layouts installed in this system.
  int size = GetKeyboardLayoutList(0, NULL);
  if (size <= 0) {
    layout = RTL_KEYBOARD_LAYOUT_ERROR;
    return false;
  }

  // Retrieve the keyboard layouts in an array and check if there is an RTL
  // layout in it.
  scoped_array<HKL> layouts(new HKL[size]);
  GetKeyboardLayoutList(size, layouts.get());
  for (int i = 0; i < size; ++i) {
    if (PRIMARYLANGID(layouts[i]) == LANG_ARABIC ||
        PRIMARYLANGID(layouts[i]) == LANG_HEBREW ||
        PRIMARYLANGID(layouts[i]) == LANG_PERSIAN) {
      layout = RTL_KEYBOARD_LAYOUT_INSTALLED;
      return true;
    }
  }

  layout = RTL_KEYBOARD_LAYOUT_NOT_INSTALLED;
  return false;
}

// Returns the text direction according to the keyboard status.
// This function retrieves the status of all keys and returns the following
// values:
// * WEB_TEXT_DIRECTION_RTL
//   If only a control key and a right-shift key are down.
// * WEB_TEXT_DIRECTION_LTR
//   If only a control key and a left-shift key are down.

static bool GetNewTextDirection(WebTextDirection* direction) {
  uint8_t keystate[256];
  if (!GetKeyboardState(&keystate[0]))
    return false;

  // To check if a user is pressing only a control key and a right-shift key
  // (or a left-shift key), we use the steps below:
  // 1. Check if a user is pressing a control key and a right-shift key (or
  //    a left-shift key).
  // 2. If the condition 1 is true, we should check if there are any other
  //    keys pressed at the same time.
  //    To ignore the keys checked in 1, we set their status to 0 before
  //    checking the key status.
  const int kKeyDownMask = 0x80;
  if ((keystate[VK_CONTROL] & kKeyDownMask) == 0)
    return false;

  if (keystate[VK_RSHIFT] & kKeyDownMask) {
    keystate[VK_RSHIFT] = 0;
    *direction = WebKit::WebTextDirectionRightToLeft;
  } else if (keystate[VK_LSHIFT] & kKeyDownMask) {
    keystate[VK_LSHIFT] = 0;
    *direction = WebKit::WebTextDirectionLeftToRight;
  } else {
    return false;
  }

  // Scan the key status to find pressed keys. We should adandon changing the
  // text direction when there are other pressed keys.
  // This code is executed only when a user is pressing a control key and a
  // right-shift key (or a left-shift key), i.e. we should ignore the status of
  // the keys: VK_SHIFT, VK_CONTROL, VK_RCONTROL, and VK_LCONTROL.
  // So, we reset their status to 0 and ignore them.
  keystate[VK_SHIFT] = 0;
  keystate[VK_CONTROL] = 0;
  keystate[VK_RCONTROL] = 0;
  keystate[VK_LCONTROL] = 0;
  for (int i = 0; i <= VK_PACKET; ++i) {
    if (keystate[i] & kKeyDownMask)
      return false;
  }
  return true;
}

class NotifyPluginProcessHostTask : public Task {
 public:
  NotifyPluginProcessHostTask(HWND window, HWND parent)
    : window_(window), parent_(parent), tries_(kMaxTries) { }

 private:
  void Run() {
    DWORD plugin_process_id;
    bool found_starting_plugin_process = false;
    GetWindowThreadProcessId(window_, &plugin_process_id);
    for (ChildProcessHost::Iterator iter(ChildProcessInfo::PLUGIN_PROCESS);
         !iter.Done(); ++iter) {
      PluginProcessHost* plugin = static_cast<PluginProcessHost*>(*iter);
      if (!plugin->handle()) {
        found_starting_plugin_process = true;
        continue;
      }
      if (base::GetProcId(plugin->handle()) == plugin_process_id) {
        plugin->AddWindow(parent_);
        return;
      }
    }

    if (found_starting_plugin_process) {
      // A plugin process has started but we don't have its handle yet.  Since
      // it's most likely the one for this plugin, try a few more times after a
      // delay.
      if (tries_--) {
        MessageLoop::current()->PostDelayedTask(FROM_HERE, this, kTryDelayMs);
        return;
      }
    }

    // The plugin process might have died in the time to execute the task, don't
    // leak the HWND.
    PostMessage(parent_, WM_CLOSE, 0, 0);
  }

  HWND window_;  // Plugin HWND, created and destroyed in the plugin process.
  HWND parent_;  // Parent HWND, created and destroyed on the browser UI thread.

  int tries_;

  // How many times we try to find a PluginProcessHost whose process matches
  // the HWND.
  static const int kMaxTries = 5;
  // How long to wait between each try.
  static const int kTryDelayMs = 200;
};

// Windows callback for OnDestroy to detach the plugin windows.
BOOL CALLBACK DetachPluginWindowsCallback(HWND window, LPARAM param) {
  if (WebPluginDelegateImpl::IsPluginDelegateWindow(window) &&
      !IsHungAppWindow(window)) {
    ::ShowWindow(window, SW_HIDE);
    SetParent(window, NULL);
  }
  return TRUE;
}

// Draw the contents of |backing_store_dc| onto |paint_rect| with a 70% grey
// filter.
void DrawDeemphasized(const gfx::Rect& paint_rect,
                      HDC backing_store_dc,
                      HDC paint_dc) {
  gfx::Canvas canvas(paint_rect.width(), paint_rect.height(), true);
  HDC dc = canvas.beginPlatformPaint();
  BitBlt(dc,
         0,
         0,
         paint_rect.width(),
         paint_rect.height(),
         backing_store_dc,
         paint_rect.x(),
         paint_rect.y(),
         SRCCOPY);
  canvas.endPlatformPaint();
  // 178 is 70% grey.
  canvas.FillRectInt(SkColorSetARGB(178, 0, 0, 0), 0, 0,
                     paint_rect.width(), paint_rect.height());
  canvas.getTopPlatformDevice().drawToHDC(paint_dc, paint_rect.x(),
                                          paint_rect.y(), NULL);

}

}  // namespace

// RenderWidgetHostView --------------------------------------------------------

// static
RenderWidgetHostView* RenderWidgetHostView::CreateViewForWidget(
    RenderWidgetHost* widget) {
  return new RenderWidgetHostViewWin(widget);
}

///////////////////////////////////////////////////////////////////////////////
// RenderWidgetHostViewWin, public:

RenderWidgetHostViewWin::RenderWidgetHostViewWin(RenderWidgetHost* widget)
    : render_widget_host_(widget),
      track_mouse_leave_(false),
      ime_notification_(false),
      capture_enter_key_(false),
      is_hidden_(false),
      about_to_validate_and_paint_(false),
      close_on_deactivate_(false),
      being_destroyed_(false),
      tooltip_hwnd_(NULL),
      tooltip_showing_(false),
      shutdown_factory_(this),
      parent_hwnd_(NULL),
      is_loading_(false),
      visually_deemphasized_(false) {
  render_widget_host_->set_view(this);
  renderer_accessible_ =
      CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableRendererAccessibility);
}

RenderWidgetHostViewWin::~RenderWidgetHostViewWin() {
  ResetTooltip();
}

void RenderWidgetHostViewWin::CreateWnd(HWND parent) {
  Create(parent);  // ATL function to create the window.
  // Uncommenting this will enable experimental out-of-process painting.
  // Contact brettw for more,
  // gpu_view_host_.reset(new GpuViewHost(render_widget_host_, m_hWnd));
}

///////////////////////////////////////////////////////////////////////////////
// RenderWidgetHostViewWin, RenderWidgetHostView implementation:

void RenderWidgetHostViewWin::InitAsPopup(
    RenderWidgetHostView* parent_host_view, const gfx::Rect& pos) {
  parent_hwnd_ = parent_host_view->GetNativeView();
  close_on_deactivate_ = true;
  Create(parent_hwnd_, NULL, NULL, WS_POPUP, WS_EX_TOOLWINDOW);
  MoveWindow(pos.x(), pos.y(), pos.width(), pos.height(), TRUE);
  ShowWindow(activatable_ ? SW_SHOW : SW_SHOWNA);
}

RenderWidgetHost* RenderWidgetHostViewWin::GetRenderWidgetHost() const {
  return render_widget_host_;
}

void RenderWidgetHostViewWin::DidBecomeSelected() {
  if (!is_hidden_)
    return;

  if (tab_switch_paint_time_.is_null())
    tab_switch_paint_time_ = TimeTicks::Now();
  is_hidden_ = false;
  EnsureTooltip();
  render_widget_host_->WasRestored();
}

void RenderWidgetHostViewWin::WasHidden() {
  if (is_hidden_)
    return;

  // If we receive any more paint messages while we are hidden, we want to
  // ignore them so we don't re-allocate the backing store.  We will paint
  // everything again when we become selected again.
  is_hidden_ = true;

  ResetTooltip();

  // If we have a renderer, then inform it that we are being hidden so it can
  // reduce its resource utilization.
  render_widget_host_->WasHidden();

  // TODO(darin): what about constrained windows?  it doesn't look like they
  // see a message when their parent is hidden.  maybe there is something more
  // generic we can do at the TabContents API level instead of relying on
  // Windows messages.
}

void RenderWidgetHostViewWin::SetSize(const gfx::Size& size) {
  if (is_hidden_)
    return;

  // No SWP_NOREDRAW as autofill popups can resize and the underneath window
  // should redraw in that case.
  UINT swp_flags = SWP_NOSENDCHANGING | SWP_NOOWNERZORDER | SWP_NOCOPYBITS |
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_DEFERERASE;
  SetWindowPos(NULL, 0, 0, size.width(), size.height(), swp_flags);
  render_widget_host_->WasResized();
  EnsureTooltip();
}

gfx::NativeView RenderWidgetHostViewWin::GetNativeView() {
  return m_hWnd;
}

void RenderWidgetHostViewWin::MovePluginWindows(
    const std::vector<WebPluginGeometry>& plugin_window_moves) {
  if (plugin_window_moves.empty())
    return;

  bool oop_plugins =
    !CommandLine::ForCurrentProcess()->HasSwitch(switches::kSingleProcess) &&
    !CommandLine::ForCurrentProcess()->HasSwitch(switches::kInProcessPlugins);

  HDWP defer_window_pos_info =
      ::BeginDeferWindowPos(static_cast<int>(plugin_window_moves.size()));

  if (!defer_window_pos_info) {
    NOTREACHED();
    return;
  }

  for (size_t i = 0; i < plugin_window_moves.size(); ++i) {
    unsigned long flags = 0;
    const WebPluginGeometry& move = plugin_window_moves[i];
    HWND window = move.window;

    // As the plugin parent window which lives on the browser UI thread is
    // destroyed asynchronously, it is possible that we have a stale window
    // sent in by the renderer for moving around.
    // Note: get the parent before checking if the window is valid, to avoid a
    // race condition where the window is destroyed after the check but before
    // the GetParent call.
    HWND parent = ::GetParent(window);
    if (!::IsWindow(window))
      continue;

    if (oop_plugins) {
      if (parent == m_hWnd) {
        // The plugin window is a direct child of this window, add an
        // intermediate window that lives on this thread to speed up scrolling.
        // Note this only works with out of process plugins since we depend on
        // PluginProcessHost to destroy the intermediate HWNDs.
        parent = ReparentWindow(window);
        ::ShowWindow(window, SW_SHOW);  // Window was created hidden.
      } else if (::GetParent(parent) != m_hWnd) {
        // The renderer should only be trying to move windows that are children
        // of its render widget window. However, this may happen as a result of
        // a race condition, so we ignore it and not kill the plugin process.
        continue;
      }

      // We move the intermediate parent window which doesn't result in cross-
      // process synchronous Windows messages.
      window = parent;
    }

    if (move.visible)
      flags |= SWP_SHOWWINDOW;
    else
      flags |= SWP_HIDEWINDOW;

    if (move.rects_valid) {
      HRGN hrgn = ::CreateRectRgn(move.clip_rect.x(),
                                  move.clip_rect.y(),
                                  move.clip_rect.right(),
                                  move.clip_rect.bottom());
      gfx::SubtractRectanglesFromRegion(hrgn, move.cutout_rects);

      // Note: System will own the hrgn after we call SetWindowRgn,
      // so we don't need to call DeleteObject(hrgn)
      ::SetWindowRgn(window, hrgn, !move.clip_rect.IsEmpty());
    } else {
      flags |= SWP_NOMOVE;
      flags |= SWP_NOSIZE;
    }

    defer_window_pos_info = ::DeferWindowPos(defer_window_pos_info,
                                             window, NULL,
                                             move.window_rect.x(),
                                             move.window_rect.y(),
                                             move.window_rect.width(),
                                             move.window_rect.height(), flags);
    if (!defer_window_pos_info) {
      DCHECK(false) << "DeferWindowPos failed, so all plugin moves ignored.";
      return;
    }
  }

  ::EndDeferWindowPos(defer_window_pos_info);
}

HWND RenderWidgetHostViewWin::ReparentWindow(HWND window) {
  static ATOM window_class = 0;
  if (!window_class) {
    WNDCLASSEX wcex;
    wcex.cbSize         = sizeof(WNDCLASSEX);
    wcex.style          = CS_DBLCLKS;
    wcex.lpfnWndProc    = ::DefWindowProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = GetModuleHandle(NULL);
    wcex.hIcon          = 0;
    wcex.hCursor        = 0;
    wcex.hbrBackground  = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    wcex.lpszMenuName   = 0;
    wcex.lpszClassName  = kWrapperNativeWindowClassName;
    wcex.hIconSm        = 0;
    window_class = RegisterClassEx(&wcex);
  }
  DCHECK(window_class);

  // TODO(apatrick): the parent window is disabled if the plugin window is
  // disabled so that mouse messages from the plugin window are passed on to the
  // browser window. This does not work for regular plugins because it prevents
  // them from receiving mouse and keyboard input. WS_DISABLED is not
  // needed when the GPU process stops using child windows for 3D rendering.
  DWORD enabled_style = ::GetWindowLong(window, GWL_STYLE) & WS_DISABLED;
  HWND parent = CreateWindowEx(
      WS_EX_LEFT | WS_EX_LTRREADING | WS_EX_RIGHTSCROLLBAR,
      MAKEINTATOM(window_class), 0,
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | enabled_style,
      0, 0, 0, 0, ::GetParent(window), 0, GetModuleHandle(NULL), 0);
  DCHECK(parent);
  ::SetParent(window, parent);
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      new NotifyPluginProcessHostTask(window, parent));
  return parent;
}

void RenderWidgetHostViewWin::Focus() {
  if (IsWindow())
    SetFocus();
}

void RenderWidgetHostViewWin::Blur() {
  views::FocusManager* focus_manager =
      views::FocusManager::GetFocusManagerForNativeView(m_hWnd);
  // We don't have a FocusManager if we are hidden.
  if (focus_manager && render_widget_host_->CanBlur())
    focus_manager->ClearFocus();
}

bool RenderWidgetHostViewWin::HasFocus() {
  return ::GetFocus() == m_hWnd;
}

void RenderWidgetHostViewWin::Show() {
  DCHECK(parent_hwnd_);
  DCHECK(parent_hwnd_ != GetDesktopWindow());
  SetParent(parent_hwnd_);
  ShowWindow(SW_SHOW);

  DidBecomeSelected();
}

void RenderWidgetHostViewWin::Hide() {
  if (GetParent() == GetDesktopWindow()) {
    LOG(WARNING) << "Hide() called twice in a row: " << this << ":" <<
        parent_hwnd_ << ":" << GetParent();
    return;
  }

  if (::GetFocus() == m_hWnd)
    ::SetFocus(NULL);
  ShowWindow(SW_HIDE);

  // Cache the old parent, then orphan the window so we stop receiving messages
  parent_hwnd_ = GetParent();
  SetParent(NULL);

  WasHidden();
}

gfx::Rect RenderWidgetHostViewWin::GetViewBounds() const {
  CRect window_rect;
  GetWindowRect(&window_rect);
  return gfx::Rect(window_rect);
}

void RenderWidgetHostViewWin::UpdateCursor(const WebCursor& cursor) {
  current_cursor_ = cursor;
  UpdateCursorIfOverSelf();
}

void RenderWidgetHostViewWin::UpdateCursorIfOverSelf() {
  static HCURSOR kCursorResizeRight = LoadCursor(NULL, IDC_SIZENWSE);
  static HCURSOR kCursorResizeLeft = LoadCursor(NULL, IDC_SIZENESW);
  static HCURSOR kCursorArrow = LoadCursor(NULL, IDC_ARROW);
  static HCURSOR kCursorAppStarting = LoadCursor(NULL, IDC_APPSTARTING);
  static HINSTANCE module_handle =
      GetModuleHandle(chrome::kBrowserResourcesDll);

  // If the mouse is over our HWND, then update the cursor state immediately.
  CPoint pt;
  GetCursorPos(&pt);
  if (WindowFromPoint(pt) == m_hWnd) {
    BOOL result = ::ScreenToClient(m_hWnd, &pt);
    DCHECK(result);
    if (render_widget_host_->GetRootWindowResizerRect().Contains(pt.x, pt.y)) {
      if (l10n_util::GetTextDirection() == l10n_util::RIGHT_TO_LEFT)
        SetCursor(kCursorResizeLeft);
      else
        SetCursor(kCursorResizeRight);
    } else {
      // We cannot pass in NULL as the module handle as this would only work for
      // standard win32 cursors. We can also receive cursor types which are
      // defined as webkit resources. We need to specify the module handle of
      // chrome.dll while loading these cursors.
      HCURSOR display_cursor = current_cursor_.GetCursor(module_handle);

      // If a page is in the loading state, we want to show the Arrow+Hourglass
      // cursor only when the current cursor is the ARROW cursor. In all other
      // cases we should continue to display the current cursor.
      if (is_loading_ && display_cursor == kCursorArrow)
        display_cursor = kCursorAppStarting;

      SetCursor(display_cursor);
    }
  }
}

void RenderWidgetHostViewWin::SetIsLoading(bool is_loading) {
  is_loading_ = is_loading;
  UpdateCursorIfOverSelf();
}

void RenderWidgetHostViewWin::IMEUpdateStatus(int control,
                                              const gfx::Rect& caret_rect) {
  if (control == IME_DISABLE) {
    ime_input_.DisableIME(m_hWnd);
  } else {
    ime_input_.EnableIME(m_hWnd, caret_rect,
                         control == IME_COMPLETE_COMPOSITION);
  }
}

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lparam) {
  if (!WebPluginDelegateImpl::IsPluginDelegateWindow(hwnd))
    return TRUE;

  gfx::Rect* rect = reinterpret_cast<gfx::Rect*>(lparam);
  static UINT msg = RegisterWindowMessage(kPaintMessageName);
  WPARAM wparam = rect->x() << 16 | rect->y();
  lparam = rect->width() << 16 | rect->height();

  // SendMessage gets the message across much quicker than PostMessage, since it
  // doesn't get queued.  When the plugin thread calls PeekMessage or other
  // Win32 APIs, sent messages are dispatched automatically.
  SendNotifyMessage(hwnd, msg, wparam, lparam);

  return TRUE;
}

void RenderWidgetHostViewWin::Redraw() {
  RECT damage_bounds;
  GetUpdateRect(&damage_bounds, FALSE);

  ScopedGDIObject<HRGN> damage_region(CreateRectRgn(0, 0, 0, 0));
  GetUpdateRgn(damage_region, FALSE);

  // Paint the invalid region synchronously.  Our caller will not paint again
  // until we return, so by painting to the screen here, we ensure effective
  // rate-limiting of backing store updates.  This helps a lot on pages that
  // have animations or fairly expensive layout (e.g., google maps).
  //
  // We paint this window synchronously, however child windows (i.e. plugins)
  // are painted asynchronously.  By avoiding synchronous cross-process window
  // message dispatching we allow scrolling to be smooth, and also avoid the
  // browser process locking up if the plugin process is hung.
  //
  RedrawWindow(NULL, damage_region, RDW_UPDATENOW | RDW_NOCHILDREN);

  // Send the invalid rect in screen coordinates.
  gfx::Rect screen_rect = GetViewBounds();
  gfx::Rect invalid_screen_rect(damage_bounds);
  invalid_screen_rect.Offset(screen_rect.x(), screen_rect.y());

  LPARAM lparam = reinterpret_cast<LPARAM>(&invalid_screen_rect);
  EnumChildWindows(m_hWnd, EnumChildProc, lparam);
}

void RenderWidgetHostViewWin::DrawResizeCorner(const gfx::Rect& paint_rect,
                                               HDC dc) {
  gfx::Rect resize_corner_rect =
      render_widget_host_->GetRootWindowResizerRect();
  if (!paint_rect.Intersect(resize_corner_rect).IsEmpty()) {
    SkBitmap* bitmap = ResourceBundle::GetSharedInstance().
        GetBitmapNamed(IDR_TEXTAREA_RESIZER);
    gfx::Canvas canvas(bitmap->width(), bitmap->height(), false);
    canvas.getDevice()->accessBitmap(true).eraseARGB(0, 0, 0, 0);
    int x = resize_corner_rect.x() + resize_corner_rect.width() -
        bitmap->width();
    bool rtl_dir = (l10n_util::GetTextDirection() ==
        l10n_util::RIGHT_TO_LEFT);
    if (rtl_dir) {
      canvas.TranslateInt(bitmap->width(), 0);
      canvas.ScaleInt(-1, 1);
      canvas.save();
      x = 0;
    }
    canvas.DrawBitmapInt(*bitmap, 0, 0);
    canvas.getTopPlatformDevice().drawToHDC(dc, x,
        resize_corner_rect.y() + resize_corner_rect.height() -
        bitmap->height(), NULL);
    if (rtl_dir)
      canvas.restore();
  }
}

void RenderWidgetHostViewWin::DidPaintBackingStoreRects(
    const std::vector<gfx::Rect>& rects) {
  if (is_hidden_)
    return;

  for (size_t i = 0; i < rects.size(); ++i)
    InvalidateRect(&rects[i].ToRECT(), false);

  if (!about_to_validate_and_paint_)
    Redraw();
}

void RenderWidgetHostViewWin::DidScrollBackingStoreRect(
    const gfx::Rect& rect, int dx, int dy) {
  if (is_hidden_)
    return;

  // We need to pass in SW_INVALIDATE to ScrollWindowEx.  The documentation on
  // MSDN states that it only applies to the HRGN argument, which is wrong.
  // Not passing in this flag does not invalidate the region which was scrolled
  // from, thus causing painting issues.
  RECT clip_rect = rect.ToRECT();
  ScrollWindowEx(dx, dy, NULL, &clip_rect, NULL, NULL, SW_INVALIDATE);
}

void RenderWidgetHostViewWin::RenderViewGone() {
  // TODO(darin): keep this around, and draw sad-tab into it.
  UpdateCursorIfOverSelf();
  being_destroyed_ = true;
  DestroyWindow();
}

void RenderWidgetHostViewWin::WillDestroyRenderWidget(RenderWidgetHost* rwh) {
  if (rwh == render_widget_host_)
    render_widget_host_ = NULL;
}

void RenderWidgetHostViewWin::Destroy() {
  // We've been told to destroy.
  // By clearing close_on_deactivate_, we prevent further deactivations
  // (caused by windows messages resulting from the DestroyWindow) from
  // triggering further destructions.  The deletion of this is handled by
  // OnFinalMessage();
  close_on_deactivate_ = false;
  being_destroyed_ = true;
  DestroyWindow();
}

void RenderWidgetHostViewWin::SetTooltipText(const std::wstring& tooltip_text) {
  // Clamp the tooltip length to kMaxTooltipLength so that we don't
  // accidentally DOS the user with a mega tooltip (since Windows doesn't seem
  // to do this itself).
  const std::wstring& new_tooltip_text =
      l10n_util::TruncateString(tooltip_text, kMaxTooltipLength);

  if (new_tooltip_text != tooltip_text_) {
    tooltip_text_ = new_tooltip_text;

    // Need to check if the tooltip is already showing so that we don't
    // immediately show the tooltip with no delay when we move the mouse from
    // a region with no tooltip to a region with a tooltip.
    if (::IsWindow(tooltip_hwnd_) && tooltip_showing_) {
      ::SendMessage(tooltip_hwnd_, TTM_POP, 0, 0);
      ::SendMessage(tooltip_hwnd_, TTM_POPUP, 0, 0);
    }
  } else {
    // Make sure the tooltip gets closed after TTN_POP gets sent. For some
    // reason this doesn't happen automatically, so moving the mouse around
    // within the same link/image/etc doesn't cause the tooltip to re-appear.
    if (!tooltip_showing_) {
      if (::IsWindow(tooltip_hwnd_))
        ::SendMessage(tooltip_hwnd_, TTM_POP, 0, 0);
    }
  }
}

BackingStore* RenderWidgetHostViewWin::AllocBackingStore(
    const gfx::Size& size) {
  if (gpu_view_host_.get())
    return gpu_view_host_->CreateBackingStore(size);
  return new BackingStoreWin(render_widget_host_, size);
}

VideoLayer* RenderWidgetHostViewWin::AllocVideoLayer(
    const gfx::Size& size) {
  NOTIMPLEMENTED();
  return NULL;
}

void RenderWidgetHostViewWin::SetBackground(const SkBitmap& background) {
  RenderWidgetHostView::SetBackground(background);
  Send(new ViewMsg_SetBackground(render_widget_host_->routing_id(),
                                 background));
}

bool RenderWidgetHostViewWin::ContainsNativeView(
    gfx::NativeView native_view) const {
  if (m_hWnd == native_view)
    return true;

  // Traverse the set of parents of the given view to determine if native_view
  // is a descendant of this window.
  HWND parent_window = ::GetParent(native_view);
  while (parent_window) {
    if (parent_window == m_hWnd)
      return true;
    parent_window = ::GetParent(parent_window);
  }

  return false;
}

void RenderWidgetHostViewWin::SetVisuallyDeemphasized(bool deemphasized) {
  if (visually_deemphasized_ == deemphasized)
    return;

  visually_deemphasized_ = deemphasized;
  InvalidateRect(NULL, FALSE);
}

///////////////////////////////////////////////////////////////////////////////
// RenderWidgetHostViewWin, private:

LRESULT RenderWidgetHostViewWin::OnCreate(CREATESTRUCT* create_struct) {
  // Call the WM_INPUTLANGCHANGE message handler to initialize the input locale
  // of a browser process.
  OnInputLangChange(0, 0);
  // Marks that window as supporting mouse-wheel messages rerouting so it is
  // scrolled when under the mouse pointer even if inactive.
  views::SetWindowSupportsRerouteMouseWheel(m_hWnd);
  // Save away our HWND in the parent window as a property so that the
  // accessibility code can find it.
  ::SetProp(GetParent(), kViewsNativeHostPropForAccessibility, m_hWnd);
  ::SetProp(m_hWnd, kRenderWidgetHostViewKey,
            static_cast<RenderWidgetHostView*>(this));
  return 0;
}

void RenderWidgetHostViewWin::OnActivate(UINT action, BOOL minimized,
                                         HWND window) {
  // If the container is a popup, clicking elsewhere on screen should close the
  // popup.
  if (close_on_deactivate_ && action == WA_INACTIVE) {
    // Send a windows message so that any derived classes
    // will get a change to override the default handling
    SendMessage(WM_CANCELMODE);
  }
}

void RenderWidgetHostViewWin::OnDestroy() {
  // When a tab is closed all its child plugin windows are destroyed
  // automatically. This happens before plugins get any notification that its
  // instances are tearing down.
  //
  // Plugins like Quicktime assume that their windows will remain valid as long
  // as they have plugin instances active. Quicktime crashes in this case
  // because its windowing code cleans up an internal data structure that the
  // handler for NPP_DestroyStream relies on.
  //
  // The fix is to detach plugin windows from web contents when it is going
  // away. This will prevent the plugin windows from getting destroyed
  // automatically. The detached plugin windows will get cleaned up in proper
  // sequence as part of the usual cleanup when the plugin instance goes away.
  EnumChildWindows(m_hWnd, DetachPluginWindowsCallback, NULL);

  ::RemoveProp(m_hWnd, kRenderWidgetHostViewKey);

  ResetTooltip();
  TrackMouseLeave(false);
}

void RenderWidgetHostViewWin::OnPaint(HDC unused_dc) {
  DCHECK(render_widget_host_->process()->HasConnection());

  if (gpu_view_host_.get()) {
    // When we're proxying painting, we don't actually display the web page
    // ourselves. We clear it white in case the proxy window isn't visible
    // yet we won't show gibberish.
    CPaintDC paint_dc(m_hWnd);
    FillRect(paint_dc.m_hDC, &paint_dc.m_ps.rcPaint,
             static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    return;
  }

  about_to_validate_and_paint_ = true;
  BackingStoreWin* backing_store = static_cast<BackingStoreWin*>(
      render_widget_host_->GetBackingStore(true));

  // We initialize |paint_dc| (and thus call BeginPaint()) after calling
  // GetBackingStore(), so that if it updates the invalid rect we'll catch the
  // changes and repaint them.
  about_to_validate_and_paint_ = false;

  // Grab the region to paint before creation of paint_dc since it clears the
  // damage region.
  ScopedGDIObject<HRGN> damage_region(CreateRectRgn(0, 0, 0, 0));
  GetUpdateRgn(damage_region, FALSE);

  CPaintDC paint_dc(m_hWnd);

  gfx::Rect damaged_rect(paint_dc.m_ps.rcPaint);
  if (damaged_rect.IsEmpty())
    return;

  if (backing_store) {
    gfx::Rect bitmap_rect(gfx::Point(), backing_store->size());

    bool manage_colors = BackingStoreWin::ColorManagementEnabled();
    if (manage_colors)
      SetICMMode(paint_dc.m_hDC, ICM_ON);

    // Blit only the damaged regions from the backing store.
    DWORD data_size = GetRegionData(damage_region, 0, NULL);
    scoped_array<char> region_data_buf(new char[data_size]);
    RGNDATA* region_data = reinterpret_cast<RGNDATA*>(region_data_buf.get());
    GetRegionData(damage_region, data_size, region_data);

    RECT* region_rects = reinterpret_cast<RECT*>(region_data->Buffer);
    for (DWORD i = 0; i < region_data->rdh.nCount; ++i) {
      gfx::Rect paint_rect = bitmap_rect.Intersect(gfx::Rect(region_rects[i]));
      if (!paint_rect.IsEmpty()) {
        DrawResizeCorner(paint_rect, backing_store->hdc());
        if (visually_deemphasized_) {
          DrawDeemphasized(paint_rect, backing_store->hdc(), paint_dc.m_hDC);
        } else {
          BitBlt(paint_dc.m_hDC,
                 paint_rect.x(),
                 paint_rect.y(),
                 paint_rect.width(),
                 paint_rect.height(),
                 backing_store->hdc(),
                 paint_rect.x(),
                 paint_rect.y(),
                 SRCCOPY);
        }
      }
    }

    if (manage_colors)
      SetICMMode(paint_dc.m_hDC, ICM_OFF);

    // Fill the remaining portion of the damaged_rect with the background
    if (damaged_rect.right() > bitmap_rect.right()) {
      RECT r;
      r.left = std::max(bitmap_rect.right(), damaged_rect.x());
      r.right = damaged_rect.right();
      r.top = damaged_rect.y();
      r.bottom = std::min(bitmap_rect.bottom(), damaged_rect.bottom());
      DrawBackground(r, &paint_dc);
    }
    if (damaged_rect.bottom() > bitmap_rect.bottom()) {
      RECT r;
      r.left = damaged_rect.x();
      r.right = damaged_rect.right();
      r.top = std::max(bitmap_rect.bottom(), damaged_rect.y());
      r.bottom = damaged_rect.bottom();
      DrawBackground(r, &paint_dc);
    }
    if (!whiteout_start_time_.is_null()) {
      TimeDelta whiteout_duration = TimeTicks::Now() - whiteout_start_time_;
      UMA_HISTOGRAM_TIMES("MPArch.RWHH_WhiteoutDuration", whiteout_duration);

      // Reset the start time to 0 so that we start recording again the next
      // time the backing store is NULL...
      whiteout_start_time_ = TimeTicks();
    }
    if (!tab_switch_paint_time_.is_null()) {
      TimeDelta tab_switch_paint_duration = TimeTicks::Now() -
          tab_switch_paint_time_;
      UMA_HISTOGRAM_TIMES("MPArch.RWH_TabSwitchPaintDuration",
          tab_switch_paint_duration);
      // Reset tab_switch_paint_time_ to 0 so future tab selections are
      // recorded.
      tab_switch_paint_time_ = TimeTicks();
    }
  } else {
    DrawBackground(paint_dc.m_ps.rcPaint, &paint_dc);
    if (whiteout_start_time_.is_null())
      whiteout_start_time_ = TimeTicks::Now();
  }
}

void RenderWidgetHostViewWin::DrawBackground(const RECT& dirty_rect,
                                             CPaintDC* dc) {
  if (!background_.empty()) {
    gfx::Canvas canvas(dirty_rect.right - dirty_rect.left,
                       dirty_rect.bottom - dirty_rect.top,
                       true);  // opaque
    canvas.TranslateInt(-dirty_rect.left, -dirty_rect.top);

    const RECT& dc_rect = dc->m_ps.rcPaint;
    canvas.TileImageInt(background_, 0, 0,
                        dc_rect.right - dc_rect.left,
                        dc_rect.bottom - dc_rect.top);

    canvas.getTopPlatformDevice().drawToHDC(*dc, dirty_rect.left,
                                            dirty_rect.top, NULL);
  } else {
    HBRUSH white_brush = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    dc->FillRect(&dirty_rect, white_brush);
  }
}

void RenderWidgetHostViewWin::OnNCPaint(HRGN update_region) {
  // Do nothing.  This suppresses the resize corner that Windows would
  // otherwise draw for us.
}

LRESULT RenderWidgetHostViewWin::OnEraseBkgnd(HDC dc) {
  return 1;
}

LRESULT RenderWidgetHostViewWin::OnSetCursor(HWND window, UINT hittest_code,
                                             UINT mouse_message_id) {
  UpdateCursorIfOverSelf();
  return 0;
}

void RenderWidgetHostViewWin::OnSetFocus(HWND window) {
  views::FocusManager::GetWidgetFocusManager()->OnWidgetFocusEvent(window,
                                                                   m_hWnd);

  if (render_widget_host_)
    render_widget_host_->GotFocus();
}

void RenderWidgetHostViewWin::OnKillFocus(HWND window) {
  views::FocusManager::GetWidgetFocusManager()->OnWidgetFocusEvent(m_hWnd,
                                                                   window);

  if (render_widget_host_)
    render_widget_host_->Blur();
}

void RenderWidgetHostViewWin::OnCaptureChanged(HWND window) {
  if (render_widget_host_)
    render_widget_host_->LostCapture();
}

void RenderWidgetHostViewWin::OnCancelMode() {
  if (render_widget_host_)
    render_widget_host_->LostCapture();

  if (close_on_deactivate_ && shutdown_factory_.empty()) {
    // Dismiss popups and menus.  We do this asynchronously to avoid changing
    // activation within this callstack, which may interfere with another window
    // being activated.  We can synchronously hide the window, but we need to
    // not change activation while doing so.
    SetWindowPos(NULL, 0, 0, 0, 0,
                 SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE |
                 SWP_NOREPOSITION | SWP_NOSIZE | SWP_NOZORDER);
    MessageLoop::current()->PostTask(FROM_HERE,
        shutdown_factory_.NewRunnableMethod(
            &RenderWidgetHostViewWin::ShutdownHost));
  }
}

void RenderWidgetHostViewWin::OnInputLangChange(DWORD character_set,
                                                HKL input_language_id) {
  // Send the given Locale ID to the ImeInput object and retrieves whether
  // or not the current input context has IMEs.
  // If the current input context has IMEs, a browser process has to send a
  // request to a renderer process that it needs status messages about
  // the focused edit control from the renderer process.
  // On the other hand, if the current input context does not have IMEs, the
  // browser process also has to send a request to the renderer process that
  // it does not need the status messages any longer.
  // To minimize the number of this notification request, we should check if
  // the browser process is actually retrieving the status messages (this
  // state is stored in ime_notification_) and send a request only if the
  // browser process has to update this status, its details are listed below:
  // * If a browser process is not retrieving the status messages,
  //   (i.e. ime_notification_ == false),
  //   send this request only if the input context does have IMEs,
  //   (i.e. ime_status == true);
  //   When it successfully sends the request, toggle its notification status,
  //   (i.e.ime_notification_ = !ime_notification_ = true).
  // * If a browser process is retrieving the status messages
  //   (i.e. ime_notification_ == true),
  //   send this request only if the input context does not have IMEs,
  //   (i.e. ime_status == false).
  //   When it successfully sends the request, toggle its notification status,
  //   (i.e.ime_notification_ = !ime_notification_ = false).
  // To analyze the above actions, we can optimize them into the ones
  // listed below:
  // 1 Sending a request only if ime_status_ != ime_notification_, and;
  // 2 Copying ime_status to ime_notification_ if it sends the request
  //   successfully (because Action 1 shows ime_status = !ime_notification_.)
  bool ime_status = ime_input_.SetInputLanguage();
  if (ime_status != ime_notification_) {
    if (Send(new ViewMsg_ImeSetInputMode(render_widget_host_->routing_id(),
                                         ime_status))) {
      ime_notification_ = ime_status;
    }
  }
}

void RenderWidgetHostViewWin::OnThemeChanged() {
  if (render_widget_host_)
    render_widget_host_->SystemThemeChanged();
}

LRESULT RenderWidgetHostViewWin::OnNotify(int w_param, NMHDR* header) {
  if (tooltip_hwnd_ == NULL)
    return 0;

  switch (header->code) {
    case TTN_GETDISPINFO: {
      NMTTDISPINFOW* tooltip_info = reinterpret_cast<NMTTDISPINFOW*>(header);
      tooltip_info->szText[0] = L'\0';
      tooltip_info->lpszText = const_cast<wchar_t*>(tooltip_text_.c_str());
      ::SendMessage(
        tooltip_hwnd_, TTM_SETMAXTIPWIDTH, 0, kTooltipMaxWidthPixels);
      SetMsgHandled(TRUE);
      break;
                          }
    case TTN_POP:
      tooltip_showing_ = false;
      SetMsgHandled(TRUE);
      break;
    case TTN_SHOW:
      tooltip_showing_ = true;
      SetMsgHandled(TRUE);
      break;
  }
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnImeSetContext(
    UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (!render_widget_host_)
    return 0;

  // We need status messages about the focused input control from a
  // renderer process when:
  //   * the current input context has IMEs, and;
  //   * an application is activated.
  // This seems to tell we should also check if the current input context has
  // IMEs before sending a request, however, this WM_IME_SETCONTEXT is
  // fortunately sent to an application only while the input context has IMEs.
  // Therefore, we just start/stop status messages according to the activation
  // status of this application without checks.
  bool activated = (wparam == TRUE);
  if (Send(new ViewMsg_ImeSetInputMode(
      render_widget_host_->routing_id(), activated))) {
    ime_notification_ = activated;
  }

  if (ime_notification_)
    ime_input_.CreateImeWindow(m_hWnd);

  ime_input_.CleanupComposition(m_hWnd);
  ime_input_.SetImeWindowStyle(m_hWnd, message, wparam, lparam, &handled);
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnImeStartComposition(
    UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (!render_widget_host_)
    return 0;

  // Reset the composition status and create IME windows.
  ime_input_.CreateImeWindow(m_hWnd);
  ime_input_.ResetComposition(m_hWnd);
  // We have to prevent WTL from calling ::DefWindowProc() because the function
  // calls ::ImmSetCompositionWindow() and ::ImmSetCandidateWindow() to
  // over-write the position of IME windows.
  handled = TRUE;
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnImeComposition(
    UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (!render_widget_host_)
    return 0;

  // At first, update the position of the IME window.
  ime_input_.UpdateImeWindow(m_hWnd);

  // Retrieve the result string and its attributes of the ongoing composition
  // and send it to a renderer process.
  ImeComposition composition;
  if (ime_input_.GetResult(m_hWnd, lparam, &composition)) {
    Send(new ViewMsg_ImeSetComposition(render_widget_host_->routing_id(),
                                       WebKit::WebCompositionCommandConfirm,
                                       composition.cursor_position,
                                       composition.target_start,
                                       composition.target_end,
                                       composition.ime_string));
    ime_input_.ResetComposition(m_hWnd);
    // Fall though and try reading the composition string.
    // Japanese IMEs send a message containing both GCS_RESULTSTR and
    // GCS_COMPSTR, which means an ongoing composition has been finished
    // by the start of another composition.
  }
  // Retrieve the composition string and its attributes of the ongoing
  // composition and send it to a renderer process.
  if (ime_input_.GetComposition(m_hWnd, lparam, &composition)) {
    Send(new ViewMsg_ImeSetComposition(render_widget_host_->routing_id(),
                                       WebKit::WebCompositionCommandSet,
                                       composition.cursor_position,
                                       composition.target_start,
                                       composition.target_end,
                                       composition.ime_string));
  }
  // We have to prevent WTL from calling ::DefWindowProc() because we do not
  // want for the IMM (Input Method Manager) to send WM_IME_CHAR messages.
  handled = TRUE;
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnImeEndComposition(
    UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (!render_widget_host_)
    return 0;

  if (ime_input_.is_composing()) {
    // A composition has been ended while there is an ongoing composition,
    // i.e. the ongoing composition has been canceled.
    // We need to reset the composition status both of the ImeInput object and
    // of the renderer process.
    std::wstring empty_string;
    Send(new ViewMsg_ImeSetComposition(render_widget_host_->routing_id(),
                                       WebKit::WebCompositionCommandDiscard,
                                       -1, -1, -1, empty_string));
    ime_input_.ResetComposition(m_hWnd);
  }
  ime_input_.DestroyImeWindow(m_hWnd);
  // Let WTL call ::DefWindowProc() and release its resources.
  handled = FALSE;
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnMouseEvent(UINT message, WPARAM wparam,
                                              LPARAM lparam, BOOL& handled) {
  handled = TRUE;

  if (::IsWindow(tooltip_hwnd_)) {
    // Forward mouse events through to the tooltip window
    MSG msg;
    msg.hwnd = m_hWnd;
    msg.message = message;
    msg.wParam = wparam;
    msg.lParam = lparam;
    SendMessage(tooltip_hwnd_, TTM_RELAYEVENT, NULL,
                reinterpret_cast<LPARAM>(&msg));
  }

  // TODO(jcampan): I am not sure if we should forward the message to the
  // TabContents first in the case of popups.  If we do, we would need to
  // convert the click from the popup window coordinates to the TabContents'
  // window coordinates. For now we don't forward the message in that case to
  // address bug #907474.
  // Note: GetParent() on popup windows returns the top window and not the
  // parent the window was created with (the parent and the owner of the popup
  // is the first non-child view of the view that was specified to the create
  // call).  So the TabContents window would have to be specified to the
  // RenderViewHostHWND as there is no way to retrieve it from the HWND.
  if (!close_on_deactivate_) {  // Don't forward if the container is a popup.
    if (message == WM_LBUTTONDOWN) {
      // If we get clicked on, where the resize corner is drawn, we delegate the
      // message to the root window, with the proper HTBOTTOMXXX wparam so that
      // Windows can take care of the resizing for us.
      if (render_widget_host_ &&
          render_widget_host_->GetRootWindowResizerRect().
              Contains(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
        WPARAM wparam = HTBOTTOMRIGHT;
        if (l10n_util::GetTextDirection() == l10n_util::RIGHT_TO_LEFT)
          wparam = HTBOTTOMLEFT;
        HWND root_hwnd = ::GetAncestor(m_hWnd, GA_ROOT);
        if (SendMessage(root_hwnd, WM_NCLBUTTONDOWN, wparam, lparam) == 0)
          return 0;
      }
    }
    switch (message) {
      case WM_LBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_MOUSEMOVE:
      case WM_MOUSELEAVE:
      case WM_RBUTTONDOWN: {
        // Give the TabContents first crack at the message. It may want to
        // prevent forwarding to the renderer if some higher level browser
        // functionality is invoked.
        LPARAM parent_msg_lparam = lparam;
        if (message != WM_MOUSELEAVE) {
          // For the messages except WM_MOUSELEAVE, before forwarding them to
          // parent window, we should adjust cursor position from client
          // coordinates in current window to client coordinates in its parent
          // window.
          CPoint cursor_pos(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
          ClientToScreen(&cursor_pos);
          GetParent().ScreenToClient(&cursor_pos);
          parent_msg_lparam = MAKELPARAM(cursor_pos.x, cursor_pos.y);
        }
        if (SendMessage(GetParent(), message, wparam, parent_msg_lparam) != 0)
          return 1;
      }
    }

    // WebKit does not update its IME status when a user clicks a mouse button
    // to change the input focus onto a popup menu. As a workaround, we finish
    // an ongoing composition every time when we click a left button.
    if (message == WM_LBUTTONDOWN)
      ime_input_.CleanupComposition(m_hWnd);
  }

  ForwardMouseEventToRenderer(message, wparam, lparam);
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnKeyEvent(UINT message, WPARAM wparam,
                                            LPARAM lparam, BOOL& handled) {
  handled = TRUE;

  // If we are a pop-up, forward tab related messages to our parent HWND, so
  // that we are dismissed appropriately and so that the focus advance in our
  // parent.
  // TODO(jcampan): http://b/issue?id=1192881 Could be abstracted in the
  //                FocusManager.
  if (close_on_deactivate_ &&
      (((message == WM_KEYDOWN || message == WM_KEYUP) && (wparam == VK_TAB)) ||
        (message == WM_CHAR && wparam == L'\t'))) {
    DCHECK(parent_hwnd_);
    // First close the pop-up.
    SendMessage(WM_CANCELMODE);
    // Then move the focus by forwarding the tab key to the parent.
    return ::SendMessage(parent_hwnd_, message, wparam, lparam);
  }

  if (!render_widget_host_)
    return 0;

  // Bug 1845: we need to update the text direction when a user releases
  // either a right-shift key or a right-control key after pressing both of
  // them. So, we just update the text direction while a user is pressing the
  // keys, and we notify the text direction when a user releases either of them.
  // Bug 9718: http://crbug.com/9718 To investigate IE and notepad, this
  // shortcut is enabled only on a PC having RTL keyboard layouts installed.
  // We should emulate them.
  if (IsRTLKeyboardLayoutInstalled()) {
    if (message == WM_KEYDOWN) {
      if (wparam == VK_SHIFT) {
        WebTextDirection direction;
        if (GetNewTextDirection(&direction))
          render_widget_host_->UpdateTextDirection(direction);
      } else if (wparam != VK_CONTROL) {
        // Bug 9762: http://crbug.com/9762 A user pressed a key except shift
        // and control keys.
        // When a user presses a key while he/she holds control and shift keys,
        // we cancel sending an IPC message in NotifyTextDirection() below and
        // ignore succeeding UpdateTextDirection() calls while we call
        // NotifyTextDirection().
        // To cancel it, this call set a flag that prevents sending an IPC
        // message in NotifyTextDirection() only if we are going to send it.
        // It is harmless to call this function if we aren't going to send it.
        render_widget_host_->CancelUpdateTextDirection();
      }
    } else if (message == WM_KEYUP &&
               (wparam == VK_SHIFT || wparam == VK_CONTROL)) {
      // We send an IPC message only if we need to update the text direction.
      render_widget_host_->NotifyTextDirection();
    }
  }

  // Special processing for enter key: When user hits enter in omnibox
  // we change focus to render host after the navigation, so repeat WM_KEYDOWNs
  // and WM_KEYUP are going to render host, despite being initiated in other
  // window. This code filters out these messages.
  bool ignore_keyboard_event = false;
  if (wparam == VK_RETURN) {
    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
      if (KF_REPEAT & HIWORD(lparam)) {
        // this is a repeated key
        if (!capture_enter_key_)
          ignore_keyboard_event = true;
      } else {
        capture_enter_key_ = true;
      }
    } else if (message == WM_KEYUP || message == WM_SYSKEYUP) {
      if (!capture_enter_key_)
        ignore_keyboard_event = true;
      capture_enter_key_ = false;
    } else {
      // Ignore all other keyboard events for the enter key if not captured.
      if (!capture_enter_key_)
        ignore_keyboard_event = true;
    }
  }

  if (render_widget_host_ && !ignore_keyboard_event) {
    render_widget_host_->ForwardKeyboardEvent(
        NativeWebKeyboardEvent(m_hWnd, message, wparam, lparam));
  }
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnWheelEvent(UINT message, WPARAM wparam,
                                              LPARAM lparam, BOOL& handled) {
  // Forward the mouse-wheel message to the window under the mouse if it belongs
  // to us.
  if (message == WM_MOUSEWHEEL &&
      views::RerouteMouseWheel(m_hWnd, wparam, lparam)) {
    handled = TRUE;
    return 0;
  }

  // Workaround for Thinkpad mousewheel driver. We get mouse wheel/scroll
  // messages even if we are not in the foreground. So here we check if
  // we have any owned popup windows in the foreground and dismiss them.
  if (m_hWnd != GetForegroundWindow()) {
    HWND toplevel_hwnd = ::GetAncestor(m_hWnd, GA_ROOT);
    EnumThreadWindows(
        GetCurrentThreadId(),
        DismissOwnedPopups,
        reinterpret_cast<LPARAM>(toplevel_hwnd));
  }

  // This is a bit of a hack, but will work for now since we don't want to
  // pollute this object with TabContents-specific functionality...
  bool handled_by_TabContents = false;
  if (GetParent()) {
    // Use a special reflected message to break recursion. If we send
    // WM_MOUSEWHEEL, the focus manager subclass of web contents will
    // route it back here.
    MSG new_message = {0};
    new_message.hwnd = m_hWnd;
    new_message.message = message;
    new_message.wParam = wparam;
    new_message.lParam = lparam;

    handled_by_TabContents =
        !!::SendMessage(GetParent(), views::kReflectedMessage, 0,
                        reinterpret_cast<LPARAM>(&new_message));
  }

  if (!handled_by_TabContents && render_widget_host_) {
    render_widget_host_->ForwardWheelEvent(
        WebInputEventFactory::mouseWheelEvent(m_hWnd, message, wparam,
                                              lparam));
  }
  handled = TRUE;
  return 0;
}

LRESULT RenderWidgetHostViewWin::OnMouseActivate(UINT, WPARAM, LPARAM,
                                                 BOOL& handled) {
  if (!activatable_)
    return MA_NOACTIVATE;

  HWND focus_window = GetFocus();
  if (!::IsWindow(focus_window) || !IsChild(focus_window)) {
    // We handle WM_MOUSEACTIVATE to set focus to the underlying plugin
    // child window. This is to ensure that keyboard events are received
    // by the plugin. The correct way to fix this would be send over
    // an event to the renderer which would then eventually send over
    // a setFocus call to the plugin widget. This would ensure that
    // the renderer (webkit) knows about the plugin widget receiving
    // focus.
    // TODO(iyengar) Do the right thing as per the above comment.
    POINT cursor_pos = {0};
    ::GetCursorPos(&cursor_pos);
    ::ScreenToClient(m_hWnd, &cursor_pos);
    HWND child_window = ::RealChildWindowFromPoint(m_hWnd, cursor_pos);
    if (::IsWindow(child_window)) {
      if (win_util::GetClassName(child_window) == kWrapperNativeWindowClassName)
        child_window = ::GetWindow(child_window, GW_CHILD);

      ::SetFocus(child_window);
      return MA_NOACTIVATE;
    }
  }
  handled = FALSE;
  return MA_ACTIVATE;
}

LRESULT RenderWidgetHostViewWin::OnGetObject(UINT message, WPARAM wparam,
                                             LPARAM lparam, BOOL& handled) {
  LRESULT reference_result = static_cast<LRESULT>(0L);
  // TODO(jcampan): http://b/issue?id=1432077 Disabling accessibility in the
  // renderer is a temporary work-around until that bug is fixed.
  if (!renderer_accessible_)
    return reference_result;

  // Accessibility readers will send an OBJID_CLIENT message.
  if (OBJID_CLIENT == lparam) {
    // If our MSAA DOM root is already created, reuse that pointer. Otherwise,
    // create a new one.
    if (!browser_accessibility_root_) {
      // Create a new instance of IAccessible. Root id is 1000, to avoid
      // conflicts with the ids used by MSAA.
      BrowserAccessibilityManager::GetInstance()->CreateAccessibilityInstance(
          IID_IAccessible, 1000,
          render_widget_host_->routing_id(),
          render_widget_host_->process()->id(),
          m_hWnd,
          reinterpret_cast<void **>(browser_accessibility_root_.Receive()));

      if (!browser_accessibility_root_) {
        // No valid root found, return with failure.
        return static_cast<LRESULT>(0L);
      }
    }

    // Create a reference to BrowserAccessibility which MSAA will marshall to
    // the client.
    reference_result = LresultFromObject(IID_IAccessible, wparam,
        static_cast<IAccessible*>(browser_accessibility_root_));
  }
  return reference_result;
}

void RenderWidgetHostViewWin::OnFinalMessage(HWND window) {
  // When the render widget host is being destroyed, it ends up calling
  // WillDestroyRenderWidget (through the RENDER_WIDGET_HOST_DESTROYED
  // notification) which NULLs render_widget_host_.
  // Note: the following bug http://crbug.com/24248 seems to report that
  // OnFinalMessage is called with a deleted |render_widget_host_|. It is not
  // clear how this could happen, hence the NULLing of render_widget_host_
  // above.
  if (!render_widget_host_ && !being_destroyed_) {
    // If you hit this NOTREACHED, please add a comment to report it on
    // http://crbug.com/24248, including what you did when it happened and if
    // you can repro.
    NOTREACHED();
  }
  if (render_widget_host_)
    render_widget_host_->ViewDestroyed();
  delete this;
}

void RenderWidgetHostViewWin::TrackMouseLeave(bool track) {
  if (track == track_mouse_leave_)
    return;
  track_mouse_leave_ = track;

  DCHECK(m_hWnd);

  TRACKMOUSEEVENT tme;
  tme.cbSize = sizeof(TRACKMOUSEEVENT);
  tme.dwFlags = TME_LEAVE;
  if (!track_mouse_leave_)
    tme.dwFlags |= TME_CANCEL;
  tme.hwndTrack = m_hWnd;

  TrackMouseEvent(&tme);
}

bool RenderWidgetHostViewWin::Send(IPC::Message* message) {
  if (!render_widget_host_)
    return false;
  return render_widget_host_->Send(message);
}

void RenderWidgetHostViewWin::EnsureTooltip() {
  UINT message = TTM_NEWTOOLRECT;

  TOOLINFO ti;
  ti.cbSize = sizeof(ti);
  ti.hwnd = m_hWnd;
  ti.uId = 0;
  if (!::IsWindow(tooltip_hwnd_)) {
    message = TTM_ADDTOOL;
    tooltip_hwnd_ = CreateWindowEx(
        WS_EX_TRANSPARENT | l10n_util::GetExtendedTooltipStyles(),
        TOOLTIPS_CLASS, NULL, TTS_NOPREFIX, 0, 0, 0, 0, m_hWnd, NULL,
        NULL, NULL);
    ti.uFlags = TTF_TRANSPARENT;
    ti.lpszText = LPSTR_TEXTCALLBACK;
  }

  CRect cr;
  GetClientRect(&ti.rect);
  SendMessage(tooltip_hwnd_, message, NULL, reinterpret_cast<LPARAM>(&ti));
}

void RenderWidgetHostViewWin::ResetTooltip() {
  if (::IsWindow(tooltip_hwnd_))
    ::DestroyWindow(tooltip_hwnd_);
  tooltip_hwnd_ = NULL;
}

void RenderWidgetHostViewWin::ForwardMouseEventToRenderer(UINT message,
                                                          WPARAM wparam,
                                                          LPARAM lparam) {
  if (!render_widget_host_)
    return;

  WebMouseEvent event(
      WebInputEventFactory::mouseEvent(m_hWnd, message, wparam, lparam));

  // Send the event to the renderer before changing mouse capture, so that the
  // capturelost event arrives after mouseup.
  render_widget_host_->ForwardMouseEvent(event);

  switch (event.type) {
    case WebInputEvent::MouseMove:
      TrackMouseLeave(true);
      break;
    case WebInputEvent::MouseLeave:
      TrackMouseLeave(false);
      break;
    case WebInputEvent::MouseDown:
      SetCapture();
      break;
    case WebInputEvent::MouseUp:
      if (GetCapture() == m_hWnd)
        ReleaseCapture();
      break;
  }

  if (activatable_ && event.type == WebInputEvent::MouseDown) {
    // This is a temporary workaround for bug 765011 to get focus when the
    // mouse is clicked. This happens after the mouse down event is sent to
    // the renderer because normally Windows does a WM_SETFOCUS after
    // WM_LBUTTONDOWN.
    SetFocus();
  }
}

void RenderWidgetHostViewWin::ShutdownHost() {
  shutdown_factory_.RevokeAll();
  if (render_widget_host_)
    render_widget_host_->Shutdown();
  // Do not touch any members at this point, |this| has been deleted.
}

// static
RenderWidgetHostView*
    RenderWidgetHostView::GetRenderWidgetHostViewFromNativeView(
        gfx::NativeView native_view) {
  if (::IsWindow(native_view)) {
    HANDLE raw_render_host_view = ::GetProp(native_view,
                                            kRenderWidgetHostViewKey);
    if (raw_render_host_view)
      return reinterpret_cast<RenderWidgetHostView*>(raw_render_host_view);
  }
  return NULL;
}
