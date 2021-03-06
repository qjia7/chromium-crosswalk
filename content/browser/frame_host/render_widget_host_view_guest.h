// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_GUEST_H_
#define CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_GUEST_H_

#include <vector>

#include "base/memory/scoped_ptr.h"
#include "content/browser/frame_host/render_widget_host_view_child_frame.h"
#include "content/common/content_export.h"
#include "ui/events/event.h"
#include "ui/events/gestures/gesture_recognizer.h"
#include "ui/events/gestures/gesture_types.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/gfx/rect.h"
#include "ui/gfx/vector2d_f.h"
#include "webkit/common/cursors/webcursor.h"

#if defined(TOOLKIT_GTK)
#include "content/browser/renderer_host/gtk_plugin_container_manager.h"
#endif  // defined(TOOLKIT_GTK)

namespace content {
class RenderWidgetHost;
class RenderWidgetHostImpl;
class BrowserPluginGuest;
struct NativeWebKeyboardEvent;

// See comments in render_widget_host_view.h about this class and its members.
// This version is for the BrowserPlugin which handles a lot of the
// functionality in a diffent place and isn't platform specific.
// The BrowserPlugin is currently a special case for out-of-process rendered
// content and therefore inherits from RenderWidgetHostViewChildFrame.
// Eventually all RenderWidgetHostViewGuest code will be subsumed by
// RenderWidgetHostViewChildFrame and this class will be removed.
//
// Some elements that are platform specific will be deal with by delegating
// the relevant calls to the platform view.
class CONTENT_EXPORT RenderWidgetHostViewGuest
    : public RenderWidgetHostViewChildFrame,
      public ui::GestureConsumer,
      public ui::GestureEventHelper {
 public:
  RenderWidgetHostViewGuest(RenderWidgetHost* widget,
                            BrowserPluginGuest* guest,
                            RenderWidgetHostView* platform_view);
  virtual ~RenderWidgetHostViewGuest();

  // RenderWidgetHostView implementation.
  virtual bool OnMessageReceived(const IPC::Message& msg) OVERRIDE;
  virtual void InitAsChild(gfx::NativeView parent_view) OVERRIDE;
  virtual void SetSize(const gfx::Size& size) OVERRIDE;
  virtual void SetBounds(const gfx::Rect& rect) OVERRIDE;
  virtual gfx::NativeView GetNativeView() const OVERRIDE;
  virtual gfx::NativeViewId GetNativeViewId() const OVERRIDE;
  virtual gfx::NativeViewAccessible GetNativeViewAccessible() OVERRIDE;
  virtual gfx::Rect GetViewBounds() const OVERRIDE;
  virtual void SetBackground(const SkBitmap& background) OVERRIDE;
  virtual gfx::Size GetPhysicalBackingSize() const OVERRIDE;

  // RenderWidgetHostViewPort implementation.
  virtual void InitAsPopup(RenderWidgetHostView* parent_host_view,
                           const gfx::Rect& pos) OVERRIDE;
  virtual void InitAsFullscreen(
      RenderWidgetHostView* reference_host_view) OVERRIDE;
  virtual void WasShown() OVERRIDE;
  virtual void WasHidden() OVERRIDE;
  virtual void MovePluginWindows(
      const gfx::Vector2d& scroll_offset,
      const std::vector<WebPluginGeometry>& moves) OVERRIDE;
  virtual void UpdateCursor(const WebCursor& cursor) OVERRIDE;
  virtual void SetIsLoading(bool is_loading) OVERRIDE;
  virtual void TextInputTypeChanged(ui::TextInputType type,
                                    ui::TextInputMode input_mode,
                                    bool can_compose_inline) OVERRIDE;
  virtual void ImeCancelComposition() OVERRIDE;
#if defined(OS_MACOSX) || defined(OS_WIN) || defined(USE_AURA)
  virtual void ImeCompositionRangeChanged(
      const gfx::Range& range,
      const std::vector<gfx::Rect>& character_bounds) OVERRIDE;
#endif
  virtual void DidUpdateBackingStore(
      const gfx::Rect& scroll_rect,
      const gfx::Vector2d& scroll_delta,
      const std::vector<gfx::Rect>& copy_rects,
      const std::vector<ui::LatencyInfo>& latency_info) OVERRIDE;
  virtual void RenderProcessGone(base::TerminationStatus status,
                                 int error_code) OVERRIDE;
  virtual void Destroy() OVERRIDE;
  virtual void SetTooltipText(const base::string16& tooltip_text) OVERRIDE;
  virtual void SelectionChanged(const base::string16& text,
                                size_t offset,
                                const gfx::Range& range) OVERRIDE;
  virtual void SelectionBoundsChanged(
      const ViewHostMsg_SelectionBounds_Params& params) OVERRIDE;
  virtual void CopyFromCompositingSurface(
      const gfx::Rect& src_subrect,
      const gfx::Size& dst_size,
      const base::Callback<void(bool, const SkBitmap&)>& callback,
      const SkBitmap::Config config) OVERRIDE;
  virtual void AcceleratedSurfaceBuffersSwapped(
      const GpuHostMsg_AcceleratedSurfaceBuffersSwapped_Params& params,
      int gpu_host_id) OVERRIDE;
  virtual void AcceleratedSurfacePostSubBuffer(
      const GpuHostMsg_AcceleratedSurfacePostSubBuffer_Params& params,
      int gpu_host_id) OVERRIDE;
  virtual void OnSwapCompositorFrame(
      uint32 output_surface_id,
      scoped_ptr<cc::CompositorFrame> frame) OVERRIDE;
  virtual void SetHasHorizontalScrollbar(
      bool has_horizontal_scrollbar) OVERRIDE;
  virtual void SetScrollOffsetPinning(
      bool is_pinned_to_left, bool is_pinned_to_right) OVERRIDE;
#if defined(OS_WIN) || defined(USE_AURA)
  virtual void ProcessAckedTouchEvent(
      const TouchEventWithLatencyInfo& touch,
      InputEventAckState ack_result) OVERRIDE;
#endif  // defined(OS_WIN) || defined(USE_AURA)
  virtual bool LockMouse() OVERRIDE;
  virtual void UnlockMouse() OVERRIDE;
  virtual void GetScreenInfo(blink::WebScreenInfo* results) OVERRIDE;

#if defined(OS_MACOSX)
  // RenderWidgetHostView implementation.
  virtual void SetActive(bool active) OVERRIDE;
  virtual void SetTakesFocusOnlyOnMouseDown(bool flag) OVERRIDE;
  virtual void SetWindowVisibility(bool visible) OVERRIDE;
  virtual void WindowFrameChanged() OVERRIDE;
  virtual void ShowDefinitionForSelection() OVERRIDE;
  virtual bool SupportsSpeech() const OVERRIDE;
  virtual void SpeakSelection() OVERRIDE;
  virtual bool IsSpeaking() const OVERRIDE;
  virtual void StopSpeaking() OVERRIDE;

  // RenderWidgetHostViewPort implementation.
  virtual bool PostProcessEventForPluginIme(
      const NativeWebKeyboardEvent& event) OVERRIDE;
#endif  // defined(OS_MACOSX)

#if defined(OS_ANDROID)
  // RenderWidgetHostViewPort implementation.
  virtual void ShowDisambiguationPopup(const gfx::Rect& target_rect,
                                       const SkBitmap& zoomed_bitmap) OVERRIDE;
  virtual void HasTouchEventHandlers(bool need_touch_events) OVERRIDE;
#endif  // defined(OS_ANDROID)

#if defined(TOOLKIT_GTK)
  virtual GdkEventButton* GetLastMouseDown() OVERRIDE;
  virtual gfx::NativeView BuildInputMethodsGtkMenu() OVERRIDE;
#endif  // defined(TOOLKIT_GTK)

#if defined(OS_WIN)
  virtual void SetParentNativeViewAccessible(
      gfx::NativeViewAccessible accessible_parent) OVERRIDE;
  virtual gfx::NativeViewId GetParentForWindowlessPlugin() const OVERRIDE;
#endif

  // Overridden from ui::GestureEventHelper.
  virtual bool CanDispatchToConsumer(ui::GestureConsumer* consumer) OVERRIDE;
  virtual void DispatchPostponedGestureEvent(ui::GestureEvent* event) OVERRIDE;
  virtual void DispatchCancelTouchEvent(ui::TouchEvent* event) OVERRIDE;

 protected:
  friend class RenderWidgetHostView;

 private:
  // Destroys this view without calling |Destroy| on |platform_view_|.
  void DestroyGuestView();

  // Builds and forwards a WebKitGestureEvent to the renderer.
  bool ForwardGestureEventToRenderer(ui::GestureEvent* gesture);

  // Process all of the given gestures (passes them on to renderer)
  void ProcessGestures(ui::GestureRecognizer::Gestures* gestures);

  BrowserPluginGuest *guest_;
  gfx::Size size_;
  // The platform view for this RenderWidgetHostView.
  // RenderWidgetHostViewGuest mostly only cares about stuff related to
  // compositing, the rest are directly forwared to this |platform_view_|.
  RenderWidgetHostViewPort* platform_view_;
#if defined(OS_WIN) || defined(USE_AURA)
  scoped_ptr<ui::GestureRecognizer> gesture_recognizer_;
#endif  // defined(OS_WIN) || defined(USE_AURA)
  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostViewGuest);
};

}  // namespace content

#endif  // CONTENT_BROWSER_FRAME_HOST_RENDER_WIDGET_HOST_VIEW_GUEST_H_
