// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/accessibility/browser_accessibility_manager_win.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlcom.h>
#include <atlcrack.h>
#include <oleacc.h>

#include "base/command_line.h"
#include "base/win/scoped_comptr.h"
#include "base/win/windows_version.h"
#include "content/browser/accessibility/browser_accessibility_state_impl.h"
#include "content/browser/accessibility/browser_accessibility_win.h"
#include "content/common/accessibility_messages.h"

namespace content {

// Some screen readers expect every tab / every unique web content container
// to be in its own HWND, like it was before Aura, but with Aura there's just
// one main HWND for a frame, or even for the whole desktop. So, we need a
// fake HWND as the root of the accessibility tree for each tab.
// We should get rid of this code when the latest two versions of all
// supported screen readers no longer make this assumption.
//
// This class implements a child HWND with zero size, that delegates its
// accessibility implementation to the root of the BrowserAccessibilityManager
// tree. This HWND is hooked up as the parent of the root object in the
// BrowserAccessibilityManager tree, so when any accessibility client
// calls ::WindowFromAccessibleObject, they get this HWND instead of the
// DesktopWindowTreeHostWin.
class AccessibleHWND
    : public ATL::CWindowImpl<AccessibleHWND,
                              ATL::CWindow,
                              ATL::CWinTraits<WS_CHILD> > {
 public:
  // Unfortunately, some screen readers look for this exact window class
  // to enable certain features. It'd be great to remove this.
  DECLARE_WND_CLASS_EX(L"Chrome_RenderWidgetHostHWND", CS_DBLCLKS, 0);

  BEGIN_MSG_MAP_EX(AccessibleHWND)
    MESSAGE_HANDLER_EX(WM_GETOBJECT, OnGetObject)
  END_MSG_MAP()

  AccessibleHWND(HWND parent, BrowserAccessibilityManagerWin* manager)
      : manager_(manager) {
    Create(parent);
    ShowWindow(true);
    MoveWindow(0, 0, 0, 0);

    HRESULT hr = ::CreateStdAccessibleObject(
        hwnd(), OBJID_WINDOW, IID_IAccessible,
        reinterpret_cast<void **>(window_accessible_.Receive()));
    DCHECK(SUCCEEDED(hr));
  }

  HWND hwnd() {
    DCHECK(::IsWindow(m_hWnd));
    return m_hWnd;
  }

  IAccessible* window_accessible() { return window_accessible_; }

  void OnManagerDeleted() {
    manager_ = NULL;
  }

 protected:
  virtual void OnFinalMessage(HWND hwnd) OVERRIDE {
    if (manager_)
      manager_->OnAccessibleHwndDeleted();
    delete this;
  }

 private:
  LRESULT OnGetObject(UINT message,
                      WPARAM w_param,
                      LPARAM l_param) {
    if (OBJID_CLIENT != l_param || !manager_)
      return static_cast<LRESULT>(0L);

    base::win::ScopedComPtr<IAccessible> root(
        manager_->GetRoot()->ToBrowserAccessibilityWin());
    return LresultFromObject(IID_IAccessible, w_param,
        static_cast<IAccessible*>(root.Detach()));
  }

  BrowserAccessibilityManagerWin* manager_;
  base::win::ScopedComPtr<IAccessible> window_accessible_;

  DISALLOW_COPY_AND_ASSIGN(AccessibleHWND);
};


// static
BrowserAccessibilityManager* BrowserAccessibilityManager::Create(
    const ui::AXNodeData& src,
    BrowserAccessibilityDelegate* delegate,
    BrowserAccessibilityFactory* factory) {
  return new BrowserAccessibilityManagerWin(
      GetDesktopWindow(), NULL, src, delegate, factory);
}

BrowserAccessibilityManagerWin*
BrowserAccessibilityManager::ToBrowserAccessibilityManagerWin() {
  return static_cast<BrowserAccessibilityManagerWin*>(this);
}

BrowserAccessibilityManagerWin::BrowserAccessibilityManagerWin(
    HWND parent_hwnd,
    IAccessible* parent_iaccessible,
    const ui::AXNodeData& src,
    BrowserAccessibilityDelegate* delegate,
    BrowserAccessibilityFactory* factory)
    : BrowserAccessibilityManager(src, delegate, factory),
      parent_hwnd_(parent_hwnd),
      parent_iaccessible_(parent_iaccessible),
      tracked_scroll_object_(NULL),
      accessible_hwnd_(NULL) {
}

BrowserAccessibilityManagerWin::~BrowserAccessibilityManagerWin() {
  if (tracked_scroll_object_) {
    tracked_scroll_object_->Release();
    tracked_scroll_object_ = NULL;
  }
  if (accessible_hwnd_)
    accessible_hwnd_->OnManagerDeleted();
}

// static
ui::AXNodeData BrowserAccessibilityManagerWin::GetEmptyDocument() {
  ui::AXNodeData empty_document;
  empty_document.id = 0;
  empty_document.role = ui::AX_ROLE_ROOT_WEB_AREA;
  empty_document.state =
      (1 << blink::WebAXStateEnabled) |
      (1 << ui::AX_STATE_READONLY) |
      (1 << ui::AX_STATE_BUSY);
  return empty_document;
}

void BrowserAccessibilityManagerWin::MaybeCallNotifyWinEvent(DWORD event,
                                                             LONG child_id) {
  // Don't fire events if this view isn't hooked up to its parent.
  if (!parent_iaccessible())
    return;

  // If on Win 7 and complete accessibility is enabled, create a fake child HWND
  // to use as the root of the accessibility tree. See comments above
  // AccessibleHWND for details.
  if (BrowserAccessibilityStateImpl::GetInstance()->IsAccessibleBrowser() &&
      !accessible_hwnd_) {
    accessible_hwnd_ = new AccessibleHWND(parent_hwnd_, this);
    parent_hwnd_ = accessible_hwnd_->hwnd();
    parent_iaccessible_ = accessible_hwnd_->window_accessible();
  }

  ::NotifyWinEvent(event, parent_hwnd(), OBJID_CLIENT, child_id);
}

void BrowserAccessibilityManagerWin::AddNodeToMap(BrowserAccessibility* node) {
  BrowserAccessibilityManager::AddNodeToMap(node);
  LONG unique_id_win = node->ToBrowserAccessibilityWin()->unique_id_win();
  unique_id_to_renderer_id_map_[unique_id_win] = node->renderer_id();
}

void BrowserAccessibilityManagerWin::RemoveNode(BrowserAccessibility* node) {
  unique_id_to_renderer_id_map_.erase(
      node->ToBrowserAccessibilityWin()->unique_id_win());
  BrowserAccessibilityManager::RemoveNode(node);
  if (node == tracked_scroll_object_) {
    tracked_scroll_object_->Release();
    tracked_scroll_object_ = NULL;
  }
}

void BrowserAccessibilityManagerWin::NotifyAccessibilityEvent(
    ui::AXEvent event_type,
    BrowserAccessibility* node) {
  if (node->role() == ui::AX_ROLE_INLINE_TEXT_BOX)
    return;

  LONG event_id = EVENT_MIN;
  switch (event_type) {
    case ui::AX_EVENT_ACTIVEDESCENDANTCHANGED:
      event_id = IA2_EVENT_ACTIVE_DESCENDANT_CHANGED;
      break;
    case ui::AX_EVENT_ALERT:
      event_id = EVENT_SYSTEM_ALERT;
      break;
    case ui::AX_EVENT_ARIA_ATTRIBUTE_CHANGED:
      event_id = IA2_EVENT_OBJECT_ATTRIBUTE_CHANGED;
      break;
    case ui::AX_EVENT_AUTOCORRECTION_OCCURED:
      event_id = IA2_EVENT_OBJECT_ATTRIBUTE_CHANGED;
      break;
    case ui::AX_EVENT_BLUR:
      // Equivalent to focus on the root.
      event_id = EVENT_OBJECT_FOCUS;
      node = GetRoot();
      break;
    case ui::AX_EVENT_CHECKED_STATE_CHANGED:
      event_id = EVENT_OBJECT_STATECHANGE;
      break;
    case ui::AX_EVENT_CHILDREN_CHANGED:
      event_id = EVENT_OBJECT_REORDER;
      break;
    case ui::AX_EVENT_FOCUS:
      event_id = EVENT_OBJECT_FOCUS;
      break;
    case ui::AX_EVENT_INVALID_STATUS_CHANGED:
      event_id = EVENT_OBJECT_STATECHANGE;
      break;
    case ui::AX_EVENT_LIVE_REGION_CHANGED:
      // TODO: try not firing a native notification at all, since
      // on Windows, each individual item in a live region that changes
      // already gets its own notification.
      event_id = EVENT_OBJECT_REORDER;
      break;
    case ui::AX_EVENT_LOAD_COMPLETE:
      event_id = IA2_EVENT_DOCUMENT_LOAD_COMPLETE;
      break;
    case ui::AX_EVENT_MENU_LIST_ITEM_SELECTED:
      event_id = EVENT_OBJECT_FOCUS;
      break;
    case ui::AX_EVENT_MENU_LIST_VALUE_CHANGED:
      event_id = EVENT_OBJECT_VALUECHANGE;
      break;
    case ui::AX_EVENT_HIDE:
      event_id = EVENT_OBJECT_HIDE;
      break;
    case ui::AX_EVENT_SHOW:
      event_id = EVENT_OBJECT_SHOW;
      break;
    case ui::AX_EVENT_SCROLLED_TO_ANCHOR:
      event_id = EVENT_SYSTEM_SCROLLINGSTART;
      break;
    case ui::AX_EVENT_SELECTED_CHILDREN_CHANGED:
      event_id = EVENT_OBJECT_SELECTIONWITHIN;
      break;
    case ui::AX_EVENT_SELECTED_TEXT_CHANGED:
      event_id = IA2_EVENT_TEXT_CARET_MOVED;
      break;
    case ui::AX_EVENT_TEXT_CHANGED:
      event_id = EVENT_OBJECT_NAMECHANGE;
      break;
    case ui::AX_EVENT_TEXT_INSERTED:
      event_id = IA2_EVENT_TEXT_INSERTED;
      break;
    case ui::AX_EVENT_TEXT_REMOVED:
      event_id = IA2_EVENT_TEXT_REMOVED;
      break;
    case ui::AX_EVENT_VALUE_CHANGED:
      event_id = EVENT_OBJECT_VALUECHANGE;
      break;
    default:
      // Not all WebKit accessibility events result in a Windows
      // accessibility notification.
      break;
  }

  if (event_id != EVENT_MIN) {
    // Pass the node's unique id in the |child_id| argument to NotifyWinEvent;
    // the AT client will then call get_accChild on the HWND's accessibility
    // object and pass it that same id, which we can use to retrieve the
    // IAccessible for this node.
    LONG child_id = node->ToBrowserAccessibilityWin()->unique_id_win();
    MaybeCallNotifyWinEvent(event_id, child_id);
  }

  // If this is a layout complete notification (sent when a container scrolls)
  // and there is a descendant tracked object, send a notification on it.
  // TODO(dmazzoni): remove once http://crbug.com/113483 is fixed.
  if (event_type == ui::AX_EVENT_LAYOUT_COMPLETE &&
      tracked_scroll_object_ &&
      tracked_scroll_object_->IsDescendantOf(node)) {
    MaybeCallNotifyWinEvent(
        IA2_EVENT_VISIBLE_DATA_CHANGED,
        tracked_scroll_object_->ToBrowserAccessibilityWin()->unique_id_win());
    tracked_scroll_object_->Release();
    tracked_scroll_object_ = NULL;
  }
}

void BrowserAccessibilityManagerWin::TrackScrollingObject(
    BrowserAccessibilityWin* node) {
  if (tracked_scroll_object_)
    tracked_scroll_object_->Release();
  tracked_scroll_object_ = node;
  tracked_scroll_object_->AddRef();
}

BrowserAccessibilityWin* BrowserAccessibilityManagerWin::GetFromUniqueIdWin(
    LONG unique_id_win) {
  base::hash_map<LONG, int32>::iterator iter =
      unique_id_to_renderer_id_map_.find(unique_id_win);
  if (iter != unique_id_to_renderer_id_map_.end()) {
    BrowserAccessibility* result = GetFromRendererID(iter->second);
    if (result)
      return result->ToBrowserAccessibilityWin();
  }
  return NULL;
}

void BrowserAccessibilityManagerWin::OnAccessibleHwndDeleted() {
  // If the AccessibleHWND is deleted, |parent_hwnd_| and
  // |parent_iaccessible_| are no longer valid either, since they were
  // derived from AccessibleHWND. We don't have to restore them to
  // previous values, though, because this should only happen
  // during the destruct sequence for this window.
  accessible_hwnd_ = NULL;
  parent_hwnd_ = NULL;
  parent_iaccessible_ = NULL;
}

}  // namespace content
