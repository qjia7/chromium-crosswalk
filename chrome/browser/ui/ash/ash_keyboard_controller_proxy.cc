// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/ash/ash_keyboard_controller_proxy.h"

#include "ash/display/display_controller.h"
#include "ash/shell.h"
#include "chrome/browser/extensions/event_names.h"
#include "chrome/browser/extensions/extension_function_dispatcher.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_system.h"
#include "chrome/browser/extensions/extension_web_contents_observer.h"
#include "chrome/browser/media/media_capture_devices_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/extensions/api/virtual_keyboard_private.h"
#include "chrome/common/extensions/extension_messages.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_view.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/view_type_utils.h"
#include "extensions/common/constants.h"
#include "ipc/ipc_message_macros.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window.h"
#include "ui/base/ime/input_method.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/keyboard/keyboard_controller.h"

namespace virtual_keyboard_private = extensions::api::virtual_keyboard_private;
typedef virtual_keyboard_private::OnTextInputBoxFocused::Context Context;

namespace {

const char* kVirtualKeyboardExtensionID = "mppnpdlheglhdfmldimlhpnegondlapf";

// The virtual keyboard show/hide animation duration.
const int kAnimationDurationMs = 200;

// The opacity of virtual keyboard container when show animation starts or
// hide animation finishes.
const float kAnimationStartOrAfterHideOpacity = 0.2f;

Context::Type TextInputTypeToGeneratedInputTypeEnum(ui::TextInputType type) {
  switch (type) {
    case ui::TEXT_INPUT_TYPE_NONE:
      return Context::TYPE_NONE;
    case ui::TEXT_INPUT_TYPE_PASSWORD:
      return Context::TYPE_PASSWORD;
    case ui::TEXT_INPUT_TYPE_EMAIL:
      return Context::TYPE_EMAIL;
    case ui::TEXT_INPUT_TYPE_NUMBER:
      return Context::TYPE_NUMBER;
    case ui::TEXT_INPUT_TYPE_TELEPHONE:
      return Context::TYPE_TEL;
    case ui::TEXT_INPUT_TYPE_URL:
      return Context::TYPE_URL;
    case ui::TEXT_INPUT_TYPE_DATE:
      return Context::TYPE_DATE;
    case ui::TEXT_INPUT_TYPE_TEXT:
    case ui::TEXT_INPUT_TYPE_SEARCH:
    case ui::TEXT_INPUT_TYPE_DATE_TIME:
    case ui::TEXT_INPUT_TYPE_DATE_TIME_LOCAL:
    case ui::TEXT_INPUT_TYPE_MONTH:
    case ui::TEXT_INPUT_TYPE_TIME:
    case ui::TEXT_INPUT_TYPE_WEEK:
    case ui::TEXT_INPUT_TYPE_TEXT_AREA:
    case ui::TEXT_INPUT_TYPE_CONTENT_EDITABLE:
    case ui::TEXT_INPUT_TYPE_DATE_TIME_FIELD:
      return Context::TYPE_TEXT;
  }
  NOTREACHED();
  return Context::TYPE_NONE;
}

}  // namespace

AshKeyboardControllerProxy::AshKeyboardControllerProxy()
  : animation_window_(NULL) {}

AshKeyboardControllerProxy::~AshKeyboardControllerProxy() {}

void AshKeyboardControllerProxy::OnRequest(
    const ExtensionHostMsg_Request_Params& params) {
  extension_function_dispatcher_->Dispatch(
      params, web_contents()->GetRenderViewHost());
}

content::BrowserContext* AshKeyboardControllerProxy::GetBrowserContext() {
  return ProfileManager::GetActiveUserProfile();
}

ui::InputMethod* AshKeyboardControllerProxy::GetInputMethod() {
  aura::Window* root_window = ash::Shell::GetInstance()->GetPrimaryRootWindow();
  DCHECK(root_window);
  return root_window->GetProperty(aura::client::kRootWindowInputMethodKey);
}

void AshKeyboardControllerProxy::RequestAudioInput(
      content::WebContents* web_contents,
      const content::MediaStreamRequest& request,
      const content::MediaResponseCallback& callback) {
  const extensions::Extension* extension = NULL;
  GURL origin(request.security_origin);
  if (origin.SchemeIs(extensions::kExtensionScheme)) {
    ExtensionService* extensions_service =
        extensions::ExtensionSystem::GetForBrowserContext(
            GetBrowserContext())->extension_service();
    extension = extensions_service->extensions()->GetByID(origin.host());
    DCHECK(extension);
  }

  MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
      web_contents, request, callback, extension);
}

void AshKeyboardControllerProxy::SetupWebContents(
    content::WebContents* contents) {
  extension_function_dispatcher_.reset(
      new ExtensionFunctionDispatcher(GetBrowserContext(), this));
  extensions::SetViewType(contents, extensions::VIEW_TYPE_VIRTUAL_KEYBOARD);
  extensions::ExtensionWebContentsObserver::CreateForWebContents(contents);
  Observe(contents);
}

extensions::WindowController*
    AshKeyboardControllerProxy::GetExtensionWindowController() const {
  // The keyboard doesn't have a window controller.
  return NULL;
}

content::WebContents*
    AshKeyboardControllerProxy::GetAssociatedWebContents() const {
  return web_contents();
}

bool AshKeyboardControllerProxy::OnMessageReceived(
    const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(AshKeyboardControllerProxy, message)
    IPC_MESSAGE_HANDLER(ExtensionHostMsg_Request, OnRequest)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void AshKeyboardControllerProxy::ShowKeyboardContainer(
    aura::Window* container) {
  // TODO(bshe): Implement logic to decide which root window should display
  // virtual keyboard. http://crbug.com/303429
  if (container->GetRootWindow() != ash::Shell::GetPrimaryRootWindow())
    NOTIMPLEMENTED();

  ui::LayerAnimator* container_animator = container->layer()->GetAnimator();
  // If the container is not animating, makes sure the position and opacity
  // are at begin states for animation.
  if (!container_animator->is_animating()) {
    gfx::Transform transform;
    transform.Translate(0, GetKeyboardWindow()->bounds().height());
    container->SetTransform(transform);
    container->layer()->SetOpacity(kAnimationStartOrAfterHideOpacity);
  }

  container_animator->set_preemption_strategy(
      ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);

  {
    // Scope the following animation settings as we don't want to animate
    // visibility change that triggered by a call to the base class function
    // ShowKeyboardContainer with these settings. The container should become
    // visible immediately.
    ui::ScopedLayerAnimationSettings settings(container_animator);
    settings.SetTweenType(gfx::Tween::EASE_IN);
    settings.SetTransitionDuration(
        base::TimeDelta::FromMilliseconds(kAnimationDurationMs));
    container->SetTransform(gfx::Transform());
    container->layer()->SetOpacity(1.0);
  }

  // TODO(bshe): Add animation observer and do the workspace resizing after
  // animation finished.
  KeyboardControllerProxy::ShowKeyboardContainer(container);
  // GetTextInputClient may return NULL when keyboard-usability-experiment flag
  // is set.
  if (GetInputMethod()->GetTextInputClient()) {
    gfx::Rect showing_area =
        ash::Shell::GetScreen()->GetPrimaryDisplay().work_area();
    GetInputMethod()->GetTextInputClient()->EnsureCaretInRect(showing_area);
  }
}

void AshKeyboardControllerProxy::HideKeyboardContainer(
    aura::Window* container) {
  // The following animation settings should persist within this function scope.
  // Otherwise, a call to base class function HideKeyboardContainer will hide
  // the container immediately.
  ui::LayerAnimator* container_animator = container->layer()->GetAnimator();
  container_animator->AddObserver(this);
  animation_window_ = container;
  ui::ScopedLayerAnimationSettings settings(container_animator);
  settings.SetTweenType(gfx::Tween::EASE_OUT);
  settings.SetTransitionDuration(
      base::TimeDelta::FromMilliseconds(kAnimationDurationMs));
  gfx::Transform transform;
  transform.Translate(0, GetKeyboardWindow()->bounds().height());
  container->SetTransform(transform);
  container->layer()->SetOpacity(kAnimationStartOrAfterHideOpacity);
}

void AshKeyboardControllerProxy::OnLayerAnimationEnded(
    ui::LayerAnimationSequence* sequence) {
  if (!animation_window_)
    return;
  ui::LayerAnimator* animator = animation_window_->layer()->GetAnimator();
  if (!animator->is_animating()) {
    KeyboardControllerProxy::HideKeyboardContainer(animation_window_);
    animator->RemoveObserver(this);
    animation_window_ = NULL;
  }
};

void AshKeyboardControllerProxy::OnLayerAnimationAborted(
    ui::LayerAnimationSequence* sequence) {
  if (!animation_window_)
    return;
  animation_window_->layer()->GetAnimator()->RemoveObserver(this);
  animation_window_ = NULL;
};

void AshKeyboardControllerProxy::SetUpdateInputType(ui::TextInputType type) {
  // TODO(bshe): Need to check the affected window's profile once multi-profile
  // is supported.
  content::BrowserContext* context = GetBrowserContext();
  extensions::EventRouter* router =
      extensions::ExtensionSystem::GetForBrowserContext(context)->
      event_router();

  if (!router->HasEventListener(
          virtual_keyboard_private::OnTextInputBoxFocused::kEventName)) {
    return;
  }

  scoped_ptr<base::ListValue> event_args(new base::ListValue());
  scoped_ptr<base::DictionaryValue> input_context(new base::DictionaryValue());
  input_context->SetString("type",
      Context::ToString(TextInputTypeToGeneratedInputTypeEnum(type)));
  event_args->Append(input_context.release());

  scoped_ptr<extensions::Event> event(new extensions::Event(
      virtual_keyboard_private::OnTextInputBoxFocused::kEventName,
      event_args.Pass()));
  event->restrict_to_browser_context = context;
  router->DispatchEventToExtension(kVirtualKeyboardExtensionID, event.Pass());
}
