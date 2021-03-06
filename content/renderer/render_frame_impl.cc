// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/render_frame_impl.h"

#include <map>
#include <string>

#include "base/command_line.h"
#include "base/debug/alias.h"
#include "base/debug/dump_without_crashing.h"
#include "base/i18n/char_iterator.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "content/child/appcache/appcache_dispatcher.h"
#include "content/child/plugin_messages.h"
#include "content/child/quota_dispatcher.h"
#include "content/child/request_extra_data.h"
#include "content/child/service_worker/web_service_worker_provider_impl.h"
#include "content/common/frame_messages.h"
#include "content/common/socket_stream_handle_data.h"
#include "content/common/swapped_out_messages.h"
#include "content/common/view_messages.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/content_renderer_client.h"
#include "content/public/renderer/document_state.h"
#include "content/public/renderer/navigation_state.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/renderer/accessibility/renderer_accessibility.h"
#include "content/renderer/browser_plugin/browser_plugin.h"
#include "content/renderer/browser_plugin/browser_plugin_manager.h"
#include "content/renderer/child_frame_compositing_helper.h"
#include "content/renderer/internal_document_state_data.h"
#include "content/renderer/npapi/plugin_channel_host.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/render_view_impl.h"
#include "content/renderer/render_widget_fullscreen_pepper.h"
#include "content/renderer/renderer_webapplicationcachehost_impl.h"
#include "content/renderer/shared_worker_repository.h"
#include "content/renderer/websharedworker_proxy.h"
#include "net/base/net_errors.h"
#include "net/http/http_util.h"
#include "third_party/WebKit/public/platform/WebStorageQuotaCallbacks.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/public/platform/WebURL.h"
#include "third_party/WebKit/public/platform/WebURLError.h"
#include "third_party/WebKit/public/platform/WebURLResponse.h"
#include "third_party/WebKit/public/platform/WebVector.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebNavigationPolicy.h"
#include "third_party/WebKit/public/web/WebPlugin.h"
#include "third_party/WebKit/public/web/WebPluginParams.h"
#include "third_party/WebKit/public/web/WebSearchableFormData.h"
#include "third_party/WebKit/public/web/WebSecurityOrigin.h"
#include "third_party/WebKit/public/web/WebUserGestureIndicator.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "webkit/child/weburlresponse_extradata_impl.h"

#if defined(ENABLE_PLUGINS)
#include "content/renderer/npapi/webplugin_impl.h"
#include "content/renderer/pepper/pepper_browser_connection.h"
#include "content/renderer/pepper/pepper_plugin_instance_impl.h"
#include "content/renderer/pepper/pepper_webplugin_impl.h"
#include "content/renderer/pepper/plugin_module.h"
#endif

#if defined(ENABLE_WEBRTC)
#include "content/renderer/media/rtc_peer_connection_handler.h"
#endif

using blink::WebDataSource;
using blink::WebDocument;
using blink::WebFrame;
using blink::WebNavigationPolicy;
using blink::WebPluginParams;
using blink::WebReferrerPolicy;
using blink::WebSearchableFormData;
using blink::WebSecurityOrigin;
using blink::WebServiceWorkerProvider;
using blink::WebStorageQuotaCallbacks;
using blink::WebString;
using blink::WebURL;
using blink::WebURLError;
using blink::WebURLRequest;
using blink::WebURLResponse;
using blink::WebUserGestureIndicator;
using blink::WebVector;
using blink::WebView;
using base::Time;
using base::TimeDelta;
using webkit_glue::WebURLResponseExtraDataImpl;

namespace content {

namespace {

typedef std::map<blink::WebFrame*, RenderFrameImpl*> FrameMap;
base::LazyInstance<FrameMap> g_frame_map = LAZY_INSTANCE_INITIALIZER;

}  // namespace

static RenderFrameImpl* (*g_create_render_frame_impl)(RenderViewImpl*, int32) =
    NULL;

// static
RenderFrameImpl* RenderFrameImpl::Create(RenderViewImpl* render_view,
                                         int32 routing_id) {
  DCHECK(routing_id != MSG_ROUTING_NONE);

  if (g_create_render_frame_impl)
    return g_create_render_frame_impl(render_view, routing_id);
  else
    return new RenderFrameImpl(render_view, routing_id);
}

// static
RenderFrame* RenderFrame::FromWebFrame(blink::WebFrame* web_frame) {
  return RenderFrameImpl::FromWebFrame(web_frame);
}

RenderFrameImpl* RenderFrameImpl::FromWebFrame(blink::WebFrame* web_frame) {
  FrameMap::iterator iter = g_frame_map.Get().find(web_frame);
  if (iter != g_frame_map.Get().end())
    return iter->second;
  return NULL;
}

// static
void RenderFrameImpl::InstallCreateHook(
    RenderFrameImpl* (*create_render_frame_impl)(RenderViewImpl*, int32)) {
  CHECK(!g_create_render_frame_impl);
  g_create_render_frame_impl = create_render_frame_impl;
}

// RenderFrameImpl ----------------------------------------------------------
RenderFrameImpl::RenderFrameImpl(RenderViewImpl* render_view, int routing_id)
    : frame_(NULL),
      render_view_(render_view->AsWeakPtr()),
      routing_id_(routing_id),
      is_swapped_out_(false),
      is_detaching_(false),
      cookie_jar_(this) {
  RenderThread::Get()->AddRoute(routing_id_, this);
}

RenderFrameImpl::~RenderFrameImpl() {
  FOR_EACH_OBSERVER(RenderFrameObserver, observers_, RenderFrameGone());
  FOR_EACH_OBSERVER(RenderFrameObserver, observers_, OnDestruct());
  RenderThread::Get()->RemoveRoute(routing_id_);
}

// TODO(nasko): Overload the delete operator to overwrite the freed
// RenderFrameImpl object and help detect potential use-after-free bug.
// See https://crbug.com/245126#c34.
void RenderFrameImpl::operator delete(void* ptr) {
  memset(ptr, 0xAF, sizeof(RenderFrameImpl));
}

void RenderFrameImpl::SetWebFrame(blink::WebFrame* web_frame) {
  DCHECK(!frame_);

  std::pair<FrameMap::iterator, bool> result = g_frame_map.Get().insert(
      std::make_pair(web_frame, this));
  CHECK(result.second) << "Inserting a duplicate item.";

  frame_ = web_frame;

#if defined(ENABLE_PLUGINS)
  new PepperBrowserConnection(this);
#endif
  new SharedWorkerRepository(this);

  // We delay calling this until we have the WebFrame so that any observer or
  // embedder can call GetWebFrame on any RenderFrame.
  GetContentClient()->renderer()->RenderFrameCreated(this);
}

RenderWidget* RenderFrameImpl::GetRenderWidget() {
  return render_view_.get();
}

#if defined(ENABLE_PLUGINS)
void RenderFrameImpl::PepperPluginCreated(RendererPpapiHost* host) {
  FOR_EACH_OBSERVER(RenderFrameObserver, observers_,
                    DidCreatePepperPlugin(host));
}

void RenderFrameImpl::PepperDidChangeCursor(
    PepperPluginInstanceImpl* instance,
    const blink::WebCursorInfo& cursor) {
  // Update the cursor appearance immediately if the requesting plugin is the
  // one which receives the last mouse event. Otherwise, the new cursor won't be
  // picked up until the plugin gets the next input event. That is bad if, e.g.,
  // the plugin would like to set an invisible cursor when there isn't any user
  // input for a while.
  if (instance == render_view_->pepper_last_mouse_event_target())
    GetRenderWidget()->didChangeCursor(cursor);
}

void RenderFrameImpl::PepperDidReceiveMouseEvent(
    PepperPluginInstanceImpl* instance) {
  render_view_->set_pepper_last_mouse_event_target(instance);
}

void RenderFrameImpl::PepperTextInputTypeChanged(
    PepperPluginInstanceImpl* instance) {
  if (instance != render_view_->focused_pepper_plugin())
    return;

  GetRenderWidget()->UpdateTextInputType();
  if (render_view_->renderer_accessibility()) {
    render_view_->renderer_accessibility()->FocusedNodeChanged(
        blink::WebNode());
  }
}

void RenderFrameImpl::PepperCaretPositionChanged(
    PepperPluginInstanceImpl* instance) {
  if (instance != render_view_->focused_pepper_plugin())
    return;
  GetRenderWidget()->UpdateSelectionBounds();
}

void RenderFrameImpl::PepperCancelComposition(
    PepperPluginInstanceImpl* instance) {
  if (instance != render_view_->focused_pepper_plugin())
    return;
  Send(new ViewHostMsg_ImeCancelComposition(render_view_->GetRoutingID()));;
#if defined(OS_MACOSX) || defined(OS_WIN) || defined(USE_AURA)
  GetRenderWidget()->UpdateCompositionInfo(true);
#endif
}

void RenderFrameImpl::PepperSelectionChanged(
    PepperPluginInstanceImpl* instance) {
  if (instance != render_view_->focused_pepper_plugin())
    return;
  render_view_->SyncSelectionIfRequired();
}

RenderWidgetFullscreenPepper* RenderFrameImpl::CreatePepperFullscreenContainer(
    PepperPluginInstanceImpl* plugin) {
  GURL active_url;
  if (render_view_->webview() && render_view_->webview()->mainFrame())
    active_url = GURL(render_view_->webview()->mainFrame()->document().url());
  RenderWidgetFullscreenPepper* widget = RenderWidgetFullscreenPepper::Create(
      GetRenderWidget()->routing_id(), plugin, active_url,
      GetRenderWidget()->screenInfo());
  widget->show(blink::WebNavigationPolicyIgnore);
  return widget;
}

bool RenderFrameImpl::IsPepperAcceptingCompositionEvents() const {
  if (!render_view_->focused_pepper_plugin())
    return false;
  return render_view_->focused_pepper_plugin()->
      IsPluginAcceptingCompositionEvents();
}

void RenderFrameImpl::PluginCrashed(const base::FilePath& plugin_path,
                                   base::ProcessId plugin_pid) {
  // TODO(jam): dispatch this IPC in RenderFrameHost and switch to use
  // routing_id_ as a result.
  Send(new FrameHostMsg_PluginCrashed(routing_id_, plugin_path, plugin_pid));
}

void RenderFrameImpl::SimulateImeSetComposition(
    const base::string16& text,
    const std::vector<blink::WebCompositionUnderline>& underlines,
    int selection_start,
    int selection_end) {
  render_view_->OnImeSetComposition(
      text, underlines, selection_start, selection_end);
}

void RenderFrameImpl::SimulateImeConfirmComposition(
    const base::string16& text,
    const gfx::Range& replacement_range) {
  render_view_->OnImeConfirmComposition(text, replacement_range, false);
}


void RenderFrameImpl::OnImeSetComposition(
    const base::string16& text,
    const std::vector<blink::WebCompositionUnderline>& underlines,
    int selection_start,
    int selection_end) {
  // When a PPAPI plugin has focus, we bypass WebKit.
  if (!IsPepperAcceptingCompositionEvents()) {
    pepper_composition_text_ = text;
  } else {
    // TODO(kinaba) currently all composition events are sent directly to
    // plugins. Use DOM event mechanism after WebKit is made aware about
    // plugins that support composition.
    // The code below mimics the behavior of WebCore::Editor::setComposition.

    // Empty -> nonempty: composition started.
    if (pepper_composition_text_.empty() && !text.empty()) {
      render_view_->focused_pepper_plugin()->HandleCompositionStart(
          base::string16());
    }
    // Nonempty -> empty: composition canceled.
    if (!pepper_composition_text_.empty() && text.empty()) {
      render_view_->focused_pepper_plugin()->HandleCompositionEnd(
          base::string16());
    }
    pepper_composition_text_ = text;
    // Nonempty: composition is ongoing.
    if (!pepper_composition_text_.empty()) {
      render_view_->focused_pepper_plugin()->HandleCompositionUpdate(
          pepper_composition_text_, underlines, selection_start,
          selection_end);
    }
  }
}

void RenderFrameImpl::OnImeConfirmComposition(
    const base::string16& text,
    const gfx::Range& replacement_range,
    bool keep_selection) {
  // When a PPAPI plugin has focus, we bypass WebKit.
  // Here, text.empty() has a special meaning. It means to commit the last
  // update of composition text (see
  // RenderWidgetHost::ImeConfirmComposition()).
  const base::string16& last_text = text.empty() ? pepper_composition_text_
                                                 : text;

  // last_text is empty only when both text and pepper_composition_text_ is.
  // Ignore it.
  if (last_text.empty())
    return;

  if (!IsPepperAcceptingCompositionEvents()) {
    base::i18n::UTF16CharIterator iterator(&last_text);
    int32 i = 0;
    while (iterator.Advance()) {
      blink::WebKeyboardEvent char_event;
      char_event.type = blink::WebInputEvent::Char;
      char_event.timeStampSeconds = base::Time::Now().ToDoubleT();
      char_event.modifiers = 0;
      char_event.windowsKeyCode = last_text[i];
      char_event.nativeKeyCode = last_text[i];

      const int32 char_start = i;
      for (; i < iterator.array_pos(); ++i) {
        char_event.text[i - char_start] = last_text[i];
        char_event.unmodifiedText[i - char_start] = last_text[i];
      }

      if (GetRenderWidget()->webwidget())
        GetRenderWidget()->webwidget()->handleInputEvent(char_event);
    }
  } else {
    // Mimics the order of events sent by WebKit.
    // See WebCore::Editor::setComposition() for the corresponding code.
    render_view_->focused_pepper_plugin()->HandleCompositionEnd(last_text);
    render_view_->focused_pepper_plugin()->HandleTextInput(last_text);
  }
  pepper_composition_text_.clear();
}

#endif  // ENABLE_PLUGINS

bool RenderFrameImpl::Send(IPC::Message* message) {
  if (is_detaching_ ||
      ((is_swapped_out_ || render_view_->is_swapped_out()) &&
       !SwappedOutMessages::CanSendWhileSwappedOut(message))) {
    delete message;
    return false;
  }

  return RenderThread::Get()->Send(message);
}

bool RenderFrameImpl::OnMessageReceived(const IPC::Message& msg) {
  ObserverListBase<RenderFrameObserver>::Iterator it(observers_);
  RenderFrameObserver* observer;
  while ((observer = it.GetNext()) != NULL) {
    if (observer->OnMessageReceived(msg))
      return true;
  }

  bool handled = true;
  bool msg_is_ok = true;
  IPC_BEGIN_MESSAGE_MAP_EX(RenderFrameImpl, msg, msg_is_ok)
    IPC_MESSAGE_HANDLER(FrameMsg_SwapOut, OnSwapOut)
    IPC_MESSAGE_HANDLER(FrameMsg_BuffersSwapped, OnBuffersSwapped)
    IPC_MESSAGE_HANDLER_GENERIC(FrameMsg_CompositorFrameSwapped,
                                OnCompositorFrameSwapped(msg))
  IPC_END_MESSAGE_MAP_EX()

  if (!msg_is_ok) {
    // The message had a handler, but its deserialization failed.
    // Kill the renderer to avoid potential spoofing attacks.
    CHECK(false) << "Unable to deserialize message in RenderFrameImpl.";
  }

  return handled;
 }

void RenderFrameImpl::OnSwapOut() {
  // Only run unload if we're not swapped out yet, but send the ack either way.
  if (!is_swapped_out_) {
    // Swap this RenderView out so the tab can navigate to a page rendered by a
    // different process.  This involves running the unload handler and clearing
    // the page.  Once WasSwappedOut is called, we also allow this process to
    // exit if there are no other active RenderViews in it.

    // Send an UpdateState message before we get swapped out.
    render_view_->SyncNavigationState();

    // Synchronously run the unload handler before sending the ACK.
    // TODO(creis): Add a WebFrame::dispatchUnloadEvent and call it here.

    // Swap out and stop sending any IPC messages that are not ACKs.
    is_swapped_out_ = true;

    // Now that we're swapped out and filtering IPC messages, stop loading to
    // ensure that no other in-progress navigation continues.  We do this here
    // to avoid sending a DidStopLoading message to the browser process.
    // TODO(creis): Should we be stopping all frames here and using
    // StopAltErrorPageFetcher with RenderView::OnStop, or just stopping this
    // frame?
    frame_->stopLoading();

    // Replace the page with a blank dummy URL. The unload handler will not be
    // run a second time, thanks to a check in FrameLoader::stopLoading.
    // TODO(creis): Need to add a better way to do this that avoids running the
    // beforeunload handler. For now, we just run it a second time silently.
    render_view_->NavigateToSwappedOutURL(frame_);

    render_view_->RegisterSwappedOutChildFrame(this);
  }

  Send(new FrameHostMsg_SwapOut_ACK(routing_id_));
}

void RenderFrameImpl::OnBuffersSwapped(
    const FrameMsg_BuffersSwapped_Params& params) {
  if (!compositing_helper_.get()) {
    compositing_helper_ =
        ChildFrameCompositingHelper::CreateCompositingHelperForRenderFrame(
            frame_, this, routing_id_);
    compositing_helper_->EnableCompositing(true);
  }
  compositing_helper_->OnBuffersSwapped(
      params.size,
      params.mailbox_name,
      params.gpu_route_id,
      params.gpu_host_id,
      render_view_->GetWebView()->deviceScaleFactor());
}

void RenderFrameImpl::OnCompositorFrameSwapped(const IPC::Message& message) {
  FrameMsg_CompositorFrameSwapped::Param param;
  if (!FrameMsg_CompositorFrameSwapped::Read(&message, &param))
    return;
  scoped_ptr<cc::CompositorFrame> frame(new cc::CompositorFrame);
  param.a.frame.AssignTo(frame.get());

  if (!compositing_helper_.get()) {
    compositing_helper_ =
        ChildFrameCompositingHelper::CreateCompositingHelperForRenderFrame(
            frame_, this, routing_id_);
    compositing_helper_->EnableCompositing(true);
  }
  compositing_helper_->OnCompositorFrameSwapped(frame.Pass(),
                                                param.a.producing_route_id,
                                                param.a.output_surface_id,
                                                param.a.producing_host_id);
}

void RenderFrameImpl::DidCommitCompositorFrame() {
  if (compositing_helper_)
    compositing_helper_->DidCommitCompositorFrame();
}

RenderView* RenderFrameImpl::GetRenderView() {
  return render_view_.get();
}

int RenderFrameImpl::GetRoutingID() {
  return routing_id_;
}

blink::WebFrame* RenderFrameImpl::GetWebFrame() {
  DCHECK(frame_);
  return frame_;
}

WebPreferences& RenderFrameImpl::GetWebkitPreferences() {
  return render_view_->GetWebkitPreferences();
}

int RenderFrameImpl::ShowContextMenu(ContextMenuClient* client,
                                     const ContextMenuParams& params) {
  return render_view_->ShowContextMenu(client, params);
}

void RenderFrameImpl::CancelContextMenu(int request_id) {
  return render_view_->CancelContextMenu(request_id);
}

blink::WebPlugin* RenderFrameImpl::CreatePlugin(
    blink::WebFrame* frame,
    const WebPluginInfo& info,
    const blink::WebPluginParams& params) {
#if defined(ENABLE_PLUGINS)
  bool pepper_plugin_was_registered = false;
  scoped_refptr<PluginModule> pepper_module(PluginModule::Create(
      this, info, &pepper_plugin_was_registered));
  if (pepper_plugin_was_registered) {
    if (pepper_module.get()) {
      return new PepperWebPluginImpl(pepper_module.get(), params, this);
    }
  }
#if defined(OS_CHROMEOS)
  LOG(WARNING) << "Pepper module/plugin creation failed.";
  return NULL;
#else
  // TODO(jam): change to take RenderFrame.
  return new WebPluginImpl(frame, params, info.path, render_view_, this);
#endif
#else
  return NULL;
#endif
}

void RenderFrameImpl::LoadURLExternally(
    blink::WebFrame* frame,
    const blink::WebURLRequest& request,
    blink::WebNavigationPolicy policy) {
  loadURLExternally(frame, request, policy);
}

// blink::WebFrameClient implementation ----------------------------------------

blink::WebPlugin* RenderFrameImpl::createPlugin(
    blink::WebFrame* frame,
    const blink::WebPluginParams& params) {
  blink::WebPlugin* plugin = NULL;
  if (GetContentClient()->renderer()->OverrideCreatePlugin(
          this, frame, params, &plugin)) {
    return plugin;
  }

  if (UTF16ToASCII(params.mimeType) == kBrowserPluginMimeType) {
    return render_view_->GetBrowserPluginManager()->CreateBrowserPlugin(
        render_view_.get(), frame);
  }

#if defined(ENABLE_PLUGINS)
  WebPluginInfo info;
  std::string mime_type;
  bool found = false;
  Send(new FrameHostMsg_GetPluginInfo(
      routing_id_, params.url, frame->top()->document().url(),
      params.mimeType.utf8(), &found, &info, &mime_type));
  if (!found)
    return NULL;

  WebPluginParams params_to_use = params;
  params_to_use.mimeType = WebString::fromUTF8(mime_type);
  return CreatePlugin(frame, info, params_to_use);
#else
  return NULL;
#endif  // defined(ENABLE_PLUGINS)
}

blink::WebMediaPlayer* RenderFrameImpl::createMediaPlayer(
    blink::WebFrame* frame,
    const blink::WebURL& url,
    blink::WebMediaPlayerClient* client) {
  // TODO(nasko): Moving the implementation here involves moving a few media
  // related client objects here or referencing them in the RenderView. Needs
  // more work to understand where the proper place for those objects is.
  return render_view_->CreateMediaPlayer(this, frame, url, client);
}

blink::WebApplicationCacheHost* RenderFrameImpl::createApplicationCacheHost(
    blink::WebFrame* frame,
    blink::WebApplicationCacheHostClient* client) {
  if (!frame || !frame->view())
    return NULL;
  return new RendererWebApplicationCacheHostImpl(
      RenderViewImpl::FromWebView(frame->view()), client,
      RenderThreadImpl::current()->appcache_dispatcher()->backend_proxy());
}

blink::WebWorkerPermissionClientProxy*
RenderFrameImpl::createWorkerPermissionClientProxy(WebFrame* frame) {
  if (!frame || !frame->view())
    return NULL;
  return GetContentClient()->renderer()->CreateWorkerPermissionClientProxy(
      this, frame);
}

blink::WebCookieJar* RenderFrameImpl::cookieJar(blink::WebFrame* frame) {
  return &cookie_jar_;
}

blink::WebServiceWorkerProvider* RenderFrameImpl::createServiceWorkerProvider(
    blink::WebFrame* frame,
    blink::WebServiceWorkerProviderClient* client) {
  return new WebServiceWorkerProviderImpl(
      ChildThread::current()->thread_safe_sender(),
      make_scoped_ptr(client));
}

void RenderFrameImpl::didAccessInitialDocument(blink::WebFrame* frame) {
  render_view_->didAccessInitialDocument(frame);
}

blink::WebFrame* RenderFrameImpl::createChildFrame(
    blink::WebFrame* parent,
    const blink::WebString& name) {
  long long child_frame_identifier = WebFrame::generateEmbedderIdentifier();
  // Synchronously notify the browser of a child frame creation to get the
  // routing_id for the RenderFrame.
  int routing_id = MSG_ROUTING_NONE;
  Send(new FrameHostMsg_CreateChildFrame(routing_id_,
                                         parent->identifier(),
                                         child_frame_identifier,
                                         base::UTF16ToUTF8(name),
                                         &routing_id));
  // Allocation of routing id failed, so we can't create a child frame. This can
  // happen if this RenderFrameImpl's IPCs are being filtered when in swapped
  // out state.
  if (routing_id == MSG_ROUTING_NONE) {
    base::debug::Alias(parent);
    base::debug::Alias(&routing_id_);
    bool render_view_is_swapped_out = GetRenderWidget()->is_swapped_out();
    base::debug::Alias(&render_view_is_swapped_out);
    bool render_view_is_closing = GetRenderWidget()->closing();
    base::debug::Alias(&render_view_is_closing);
    base::debug::Alias(&is_swapped_out_);
    base::debug::DumpWithoutCrashing();
    return NULL;
  }

  RenderFrameImpl* child_render_frame = RenderFrameImpl::Create(
      render_view_.get(), routing_id);
  // TODO(nasko): Over-conservative check for debugging.
  CHECK(child_render_frame);
  blink::WebFrame* web_frame = WebFrame::create(child_render_frame,
                                                child_frame_identifier);
  // TODO(nasko): Over-conservative check for debugging.
  CHECK(web_frame);
  child_render_frame->SetWebFrame(web_frame);

  return web_frame;
}

void RenderFrameImpl::didDisownOpener(blink::WebFrame* frame) {
  render_view_->didDisownOpener(frame);
}

void RenderFrameImpl::frameDetached(blink::WebFrame* frame) {
  // NOTE: This function is called on the frame that is being detached and not
  // the parent frame.  This is different from createChildFrame() which is
  // called on the parent frame.
  CHECK(!is_detaching_);
  // TODO(nasko): Remove all debug::Alias lines after diagnosing failures.
  base::debug::Alias(frame);

  bool is_subframe = !!frame->parent();
  base::debug::Alias(&is_subframe);

  int64 parent_frame_id = -1;
  base::debug::Alias(&parent_frame_id);
  if (is_subframe)
    parent_frame_id = frame->parent()->identifier();

  Send(new FrameHostMsg_Detach(routing_id_, parent_frame_id,
                               frame->identifier()));

  render_view_->UnregisterSwappedOutChildFrame(this);

  // The |is_detaching_| flag disables Send(). FrameHostMsg_Detach must be
  // sent before setting |is_detaching_| to true. In contrast, Observers
  // should only be notified afterwards so they cannot call back into here and
  // have IPCs fired off.
  is_detaching_ = true;

  // Call back to RenderViewImpl for observers to be notified.
  // TODO(nasko): Remove once we have RenderFrameObserver.
  render_view_->frameDetached(frame);

  // We need to clean up subframes by removing them from the map and deleting
  // the RenderFrameImpl.  In contrast, the main frame is owned by its
  // containing RenderViewHost (so that they have the same lifetime), so only
  // removal from the map is needed and no deletion.
  FrameMap::iterator it = g_frame_map.Get().find(frame);
  CHECK(it != g_frame_map.Get().end());
  CHECK_EQ(it->second, this);
  g_frame_map.Get().erase(it);

  // |frame| is invalid after here.
  frame->close();

  if (is_subframe) {
    delete this;
    // Object is invalid after this point.
  }
}

void RenderFrameImpl::willClose(blink::WebFrame* frame) {
  // Call back to RenderViewImpl for observers to be notified.
  // TODO(nasko): Remove once we have RenderFrameObserver.
  render_view_->willClose(frame);
}

void RenderFrameImpl::didChangeName(blink::WebFrame* frame,
                                    const blink::WebString& name) {
  if (!render_view_->renderer_preferences_.report_frame_name_changes)
    return;

  render_view_->Send(
      new ViewHostMsg_UpdateFrameName(render_view_->GetRoutingID(),
                                      frame->identifier(),
                                      !frame->parent(),
                                      base::UTF16ToUTF8(name)));
}

void RenderFrameImpl::didMatchCSS(
    blink::WebFrame* frame,
    const blink::WebVector<blink::WebString>& newly_matching_selectors,
    const blink::WebVector<blink::WebString>& stopped_matching_selectors) {
  render_view_->didMatchCSS(
      frame, newly_matching_selectors, stopped_matching_selectors);
}

void RenderFrameImpl::loadURLExternally(blink::WebFrame* frame,
                                        const blink::WebURLRequest& request,
                                        blink::WebNavigationPolicy policy) {
  loadURLExternally(frame, request, policy, WebString());
}

void RenderFrameImpl::loadURLExternally(
    blink::WebFrame* frame,
    const blink::WebURLRequest& request,
    blink::WebNavigationPolicy policy,
    const blink::WebString& suggested_name) {
  Referrer referrer(RenderViewImpl::GetReferrerFromRequest(frame, request));
  if (policy == blink::WebNavigationPolicyDownload) {
    render_view_->Send(new ViewHostMsg_DownloadUrl(render_view_->GetRoutingID(),
                                                   request.url(), referrer,
                                                   suggested_name));
  } else {
    render_view_->OpenURL(frame, request.url(), referrer, policy);
  }
}

blink::WebNavigationPolicy RenderFrameImpl::decidePolicyForNavigation(
    blink::WebFrame* frame,
    blink::WebDataSource::ExtraData* extra_data,
    const blink::WebURLRequest& request,
    blink::WebNavigationType type,
    blink::WebNavigationPolicy default_policy,
    bool is_redirect) {
  return render_view_->DecidePolicyForNavigation(
      this, frame, extra_data, request, type, default_policy, is_redirect);
}

blink::WebNavigationPolicy RenderFrameImpl::decidePolicyForNavigation(
    blink::WebFrame* frame,
    const blink::WebURLRequest& request,
    blink::WebNavigationType type,
    blink::WebNavigationPolicy default_policy,
    bool is_redirect) {
  return decidePolicyForNavigation(frame,
                                   frame->provisionalDataSource()->extraData(),
                                   request, type, default_policy, is_redirect);
}

void RenderFrameImpl::willSendSubmitEvent(blink::WebFrame* frame,
                                          const blink::WebFormElement& form) {
  // Call back to RenderViewImpl for observers to be notified.
  // TODO(nasko): Remove once we have RenderFrameObserver.
  render_view_->willSendSubmitEvent(frame, form);
}

void RenderFrameImpl::willSubmitForm(blink::WebFrame* frame,
                                     const blink::WebFormElement& form) {
  DocumentState* document_state =
      DocumentState::FromDataSource(frame->provisionalDataSource());
  NavigationState* navigation_state = document_state->navigation_state();
  InternalDocumentStateData* internal_data =
      InternalDocumentStateData::FromDocumentState(document_state);

  if (PageTransitionCoreTypeIs(navigation_state->transition_type(),
                               PAGE_TRANSITION_LINK)) {
    navigation_state->set_transition_type(PAGE_TRANSITION_FORM_SUBMIT);
  }

  // Save these to be processed when the ensuing navigation is committed.
  WebSearchableFormData web_searchable_form_data(form);
  internal_data->set_searchable_form_url(web_searchable_form_data.url());
  internal_data->set_searchable_form_encoding(
      web_searchable_form_data.encoding().utf8());

  // Call back to RenderViewImpl for observers to be notified.
  // TODO(nasko): Remove once we have RenderFrameObserver.
  render_view_->willSubmitForm(frame, form);
}

void RenderFrameImpl::didCreateDataSource(blink::WebFrame* frame,
                                          blink::WebDataSource* datasource) {
  // TODO(nasko): Move implementation here. Needed state:
  // * pending_navigation_params_
  // * webview
  // Needed methods:
  // * PopulateDocumentStateFromPending
  // * CreateNavigationStateFromPending
  render_view_->didCreateDataSource(frame, datasource);
}

void RenderFrameImpl::didStartProvisionalLoad(blink::WebFrame* frame) {
  WebDataSource* ds = frame->provisionalDataSource();

  // In fast/loader/stop-provisional-loads.html, we abort the load before this
  // callback is invoked.
  if (!ds)
    return;

  DocumentState* document_state = DocumentState::FromDataSource(ds);

  // We should only navigate to swappedout:// when is_swapped_out_ is true.
  CHECK((ds->request().url() != GURL(kSwappedOutURL)) ||
        is_swapped_out_ ||
        render_view_->is_swapped_out()) <<
        "Heard swappedout:// when not swapped out.";

  // Update the request time if WebKit has better knowledge of it.
  if (document_state->request_time().is_null()) {
    double event_time = ds->triggeringEventTime();
    if (event_time != 0.0)
      document_state->set_request_time(Time::FromDoubleT(event_time));
  }

  // Start time is only set after request time.
  document_state->set_start_load_time(Time::Now());

  bool is_top_most = !frame->parent();
  if (is_top_most) {
    render_view_->set_navigation_gesture(
        WebUserGestureIndicator::isProcessingUserGesture() ?
            NavigationGestureUser : NavigationGestureAuto);
  } else if (ds->replacesCurrentHistoryItem()) {
    // Subframe navigations that don't add session history items must be
    // marked with AUTO_SUBFRAME. See also didFailProvisionalLoad for how we
    // handle loading of error pages.
    document_state->navigation_state()->set_transition_type(
        PAGE_TRANSITION_AUTO_SUBFRAME);
  }

  FOR_EACH_OBSERVER(
      RenderViewObserver, render_view_->observers(),
      DidStartProvisionalLoad(frame));

  Send(new FrameHostMsg_DidStartProvisionalLoadForFrame(
       routing_id_, frame->identifier(),
       frame->parent() ? frame->parent()->identifier() : -1,
       is_top_most, ds->request().url()));
}

void RenderFrameImpl::didReceiveServerRedirectForProvisionalLoad(
    blink::WebFrame* frame) {
  if (frame->parent())
    return;
  // Received a redirect on the main frame.
  WebDataSource* data_source = frame->provisionalDataSource();
  if (!data_source) {
    // Should only be invoked when we have a data source.
    NOTREACHED();
    return;
  }
  std::vector<GURL> redirects;
  RenderViewImpl::GetRedirectChain(data_source, &redirects);
  if (redirects.size() >= 2) {
    Send(new FrameHostMsg_DidRedirectProvisionalLoad(
        routing_id_,
        render_view_->page_id_,
        redirects[redirects.size() - 2],
        redirects.back()));
  }
}

void RenderFrameImpl::didFailProvisionalLoad(
    blink::WebFrame* frame,
    const blink::WebURLError& error) {
  WebDataSource* ds = frame->provisionalDataSource();
  DCHECK(ds);

  const WebURLRequest& failed_request = ds->request();

  // Call out to RenderViewImpl, so observers are notified.
  render_view_->didFailProvisionalLoad(frame, error);

  bool show_repost_interstitial =
      (error.reason == net::ERR_CACHE_MISS &&
       EqualsASCII(failed_request.httpMethod(), "POST"));

  FrameHostMsg_DidFailProvisionalLoadWithError_Params params;
  params.frame_id = frame->identifier();
  params.frame_unique_name = frame->uniqueName();
  params.is_main_frame = !frame->parent();
  params.error_code = error.reason;
  GetContentClient()->renderer()->GetNavigationErrorStrings(
      render_view_.get(),
      frame,
      failed_request,
      error,
      NULL,
      &params.error_description);
  params.url = error.unreachableURL;
  params.showing_repost_interstitial = show_repost_interstitial;
  Send(new FrameHostMsg_DidFailProvisionalLoadWithError(
      routing_id_, params));

  // Don't display an error page if this is simply a cancelled load.  Aside
  // from being dumb, WebCore doesn't expect it and it will cause a crash.
  if (error.reason == net::ERR_ABORTED)
    return;

  // Don't display "client blocked" error page if browser has asked us not to.
  if (error.reason == net::ERR_BLOCKED_BY_CLIENT &&
      render_view_->renderer_preferences_.disable_client_blocked_error_page) {
    return;
  }

  // Allow the embedder to suppress an error page.
  if (GetContentClient()->renderer()->ShouldSuppressErrorPage(this,
          error.unreachableURL)) {
    return;
  }

  if (RenderThreadImpl::current() &&
      RenderThreadImpl::current()->layout_test_mode()) {
    return;
  }

  // Make sure we never show errors in view source mode.
  frame->enableViewSourceMode(false);

  DocumentState* document_state = DocumentState::FromDataSource(ds);
  NavigationState* navigation_state = document_state->navigation_state();

  // If this is a failed back/forward/reload navigation, then we need to do a
  // 'replace' load.  This is necessary to avoid messing up session history.
  // Otherwise, we do a normal load, which simulates a 'go' navigation as far
  // as session history is concerned.
  //
  // AUTO_SUBFRAME loads should always be treated as loads that do not advance
  // the page id.
  //
  // TODO(davidben): This should also take the failed navigation's replacement
  // state into account, if a location.replace() failed.
  bool replace =
      navigation_state->pending_page_id() != -1 ||
      PageTransitionCoreTypeIs(navigation_state->transition_type(),
                               PAGE_TRANSITION_AUTO_SUBFRAME);

  // If we failed on a browser initiated request, then make sure that our error
  // page load is regarded as the same browser initiated request.
  if (!navigation_state->is_content_initiated()) {
    render_view_->pending_navigation_params_.reset(new ViewMsg_Navigate_Params);
    render_view_->pending_navigation_params_->page_id =
        navigation_state->pending_page_id();
    render_view_->pending_navigation_params_->pending_history_list_offset =
        navigation_state->pending_history_list_offset();
    render_view_->pending_navigation_params_->should_clear_history_list =
        navigation_state->history_list_was_cleared();
    render_view_->pending_navigation_params_->transition =
        navigation_state->transition_type();
    render_view_->pending_navigation_params_->request_time =
        document_state->request_time();
    render_view_->pending_navigation_params_->should_replace_current_entry =
        replace;
  }

  // Load an error page.
  render_view_->LoadNavigationErrorPage(
      frame, failed_request, error, replace);
}

void RenderFrameImpl::didCommitProvisionalLoad(blink::WebFrame* frame,
                                               bool is_new_navigation) {
  // TODO(nasko): Move implementation here. Needed state:
  // * page_id_
  // * next_page_id_
  // * history_list_offset_
  // * history_list_length_
  // * history_page_ids_
  // Needed methods
  // * webview
  // * UpdateSessionHistory
  // * GetLoadingUrl
  render_view_->didCommitProvisionalLoad(frame, is_new_navigation);

  FOR_EACH_OBSERVER(RenderFrameObserver, observers_,
                    DidCommitProvisionalLoad(is_new_navigation));
}

void RenderFrameImpl::didClearWindowObject(blink::WebFrame* frame,
                                           int world_id) {
  // TODO(nasko): Move implementation here. Needed state:
  // * enabled_bindings_
  // * dom_automation_controller_
  // * stats_collection_controller_
  render_view_->didClearWindowObject(frame, world_id);
}

void RenderFrameImpl::didCreateDocumentElement(blink::WebFrame* frame) {
  // Notify the browser about non-blank documents loading in the top frame.
  GURL url = frame->document().url();
  if (url.is_valid() && url.spec() != kAboutBlankURL) {
    // TODO(nasko): Check if webview()->mainFrame() is the same as the
    // frame->tree()->top().
    if (frame == render_view_->webview()->mainFrame()) {
      render_view_->Send(new ViewHostMsg_DocumentAvailableInMainFrame(
          render_view_->GetRoutingID()));
    }
  }

  // Call back to RenderViewImpl for observers to be notified.
  // TODO(nasko): Remove once we have RenderFrameObserver.
  render_view_->didCreateDocumentElement(frame);
}

void RenderFrameImpl::didReceiveTitle(blink::WebFrame* frame,
                                      const blink::WebString& title,
                                      blink::WebTextDirection direction) {
  // TODO(nasko): Investigate wheather implementation should move here.
  render_view_->didReceiveTitle(frame, title, direction);
}

void RenderFrameImpl::didChangeIcon(blink::WebFrame* frame,
                                    blink::WebIconURL::Type icon_type) {
  // TODO(nasko): Investigate wheather implementation should move here.
  render_view_->didChangeIcon(frame, icon_type);
}

void RenderFrameImpl::didFinishDocumentLoad(blink::WebFrame* frame) {
  // TODO(nasko): Move implementation here. No state needed, just observers
  // notification in before updating encoding.
  render_view_->didFinishDocumentLoad(frame);
}

void RenderFrameImpl::didHandleOnloadEvents(blink::WebFrame* frame) {
  // TODO(nasko): Move implementation here. Needed state:
  // * page_id_
  render_view_->didHandleOnloadEvents(frame);
}

void RenderFrameImpl::didFailLoad(blink::WebFrame* frame,
                                  const blink::WebURLError& error) {
  // TODO(nasko): Move implementation here. No state needed.
  render_view_->didFailLoad(frame, error);
}

void RenderFrameImpl::didFinishLoad(blink::WebFrame* frame) {
  // TODO(nasko): Move implementation here. No state needed, just observers
  // notification before sending message to the browser process.
  render_view_->didFinishLoad(frame);
}

void RenderFrameImpl::didNavigateWithinPage(blink::WebFrame* frame,
                                            bool is_new_navigation) {
  // TODO(nasko): Move implementation here. No state needed, just observers
  // notification before sending message to the browser process.
  render_view_->didNavigateWithinPage(frame, is_new_navigation);
}

void RenderFrameImpl::didUpdateCurrentHistoryItem(blink::WebFrame* frame) {
  // TODO(nasko): Move implementation here. Needed methods:
  // * StartNavStateSyncTimerIfNecessary
  render_view_->didUpdateCurrentHistoryItem(frame);
}

void RenderFrameImpl::willRequestAfterPreconnect(
    blink::WebFrame* frame,
    blink::WebURLRequest& request) {
  blink::WebReferrerPolicy referrer_policy = frame->document().referrerPolicy();
  // FIXME(kohei): This will never be set.
  WebString custom_user_agent;

  DCHECK(!request.extraData());

  bool was_after_preconnect_request = true;
  // The args after |was_after_preconnect_request| are not used, and set to
  // correct values at |willSendRequest|.
  request.setExtraData(new webkit_glue::WebURLRequestExtraDataImpl(
      referrer_policy, custom_user_agent, was_after_preconnect_request));
}

void RenderFrameImpl::willSendRequest(
    blink::WebFrame* frame,
    unsigned identifier,
    blink::WebURLRequest& request,
    const blink::WebURLResponse& redirect_response) {
  // The request my be empty during tests.
  if (request.url().isEmpty())
    return;

  WebFrame* top_frame = frame->top();
  if (!top_frame)
    top_frame = frame;
  WebDataSource* provisional_data_source = top_frame->provisionalDataSource();
  WebDataSource* top_data_source = top_frame->dataSource();
  WebDataSource* data_source =
      provisional_data_source ? provisional_data_source : top_data_source;

  PageTransition transition_type = PAGE_TRANSITION_LINK;
  DocumentState* document_state = DocumentState::FromDataSource(data_source);
  DCHECK(document_state);
  InternalDocumentStateData* internal_data =
      InternalDocumentStateData::FromDocumentState(document_state);
  NavigationState* navigation_state = document_state->navigation_state();
  transition_type = navigation_state->transition_type();

  GURL request_url(request.url());
  GURL new_url;
  if (GetContentClient()->renderer()->WillSendRequest(
          frame,
          transition_type,
          request_url,
          request.firstPartyForCookies(),
          &new_url)) {
    request.setURL(WebURL(new_url));
  }

  if (internal_data->is_cache_policy_override_set())
    request.setCachePolicy(internal_data->cache_policy_override());

  blink::WebReferrerPolicy referrer_policy;
  if (internal_data->is_referrer_policy_set()) {
    referrer_policy = internal_data->referrer_policy();
  } else {
    referrer_policy = frame->document().referrerPolicy();
  }

  // The request's extra data may indicate that we should set a custom user
  // agent. This needs to be done here, after WebKit is through with setting the
  // user agent on its own.
  WebString custom_user_agent;
  bool was_after_preconnect_request = false;
  if (request.extraData()) {
    webkit_glue::WebURLRequestExtraDataImpl* old_extra_data =
        static_cast<webkit_glue::WebURLRequestExtraDataImpl*>(
            request.extraData());
    custom_user_agent = old_extra_data->custom_user_agent();
    was_after_preconnect_request =
        old_extra_data->was_after_preconnect_request();

    if (!custom_user_agent.isNull()) {
      if (custom_user_agent.isEmpty())
        request.clearHTTPHeaderField("User-Agent");
      else
        request.setHTTPHeaderField("User-Agent", custom_user_agent);
    }
  }

  // Attach |should_replace_current_entry| state to requests so that, should
  // this navigation later require a request transfer, all state is preserved
  // when it is re-created in the new process.
  bool should_replace_current_entry = false;
  if (navigation_state->is_content_initiated()) {
    should_replace_current_entry = data_source->replacesCurrentHistoryItem();
  } else {
    // If the navigation is browser-initiated, the NavigationState contains the
    // correct value instead of the WebDataSource.
    //
    // TODO(davidben): Avoid this awkward duplication of state. See comment on
    // NavigationState::should_replace_current_entry().
    should_replace_current_entry =
        navigation_state->should_replace_current_entry();
  }
  request.setExtraData(
      new RequestExtraData(referrer_policy,
                           render_view_->visibilityState(),
                           custom_user_agent,
                           was_after_preconnect_request,
                           routing_id_,
                           (frame == top_frame),
                           frame->identifier(),
                           GURL(frame->document().securityOrigin().toString()),
                           frame->parent() == top_frame,
                           frame->parent() ? frame->parent()->identifier() : -1,
                           navigation_state->allow_download(),
                           transition_type,
                           should_replace_current_entry,
                           navigation_state->transferred_request_child_id(),
                           navigation_state->transferred_request_request_id()));

  DocumentState* top_document_state =
      DocumentState::FromDataSource(top_data_source);
  if (top_document_state) {
    // TODO(gavinp): separate out prefetching and prerender field trials
    // if the rel=prerender rel type is sticking around.
    if (request.targetType() == WebURLRequest::TargetIsPrefetch)
      top_document_state->set_was_prefetcher(true);

    if (was_after_preconnect_request)
      top_document_state->set_was_after_preconnect_request(true);
  }

  // This is an instance where we embed a copy of the routing id
  // into the data portion of the message. This can cause problems if we
  // don't register this id on the browser side, since the download manager
  // expects to find a RenderViewHost based off the id.
  request.setRequestorID(render_view_->GetRoutingID());
  request.setHasUserGesture(WebUserGestureIndicator::isProcessingUserGesture());

  if (!navigation_state->extra_headers().empty()) {
    for (net::HttpUtil::HeadersIterator i(
        navigation_state->extra_headers().begin(),
        navigation_state->extra_headers().end(), "\n");
        i.GetNext(); ) {
      request.setHTTPHeaderField(WebString::fromUTF8(i.name()),
                                 WebString::fromUTF8(i.values()));
    }
  }

  if (!render_view_->renderer_preferences_.enable_referrers)
    request.clearHTTPHeaderField("Referer");
}

void RenderFrameImpl::didReceiveResponse(
    blink::WebFrame* frame,
    unsigned identifier,
    const blink::WebURLResponse& response) {
  // Only do this for responses that correspond to a provisional data source
  // of the top-most frame.  If we have a provisional data source, then we
  // can't have any sub-resources yet, so we know that this response must
  // correspond to a frame load.
  if (!frame->provisionalDataSource() || frame->parent())
    return;

  // If we are in view source mode, then just let the user see the source of
  // the server's error page.
  if (frame->isViewSourceModeEnabled())
    return;

  DocumentState* document_state =
      DocumentState::FromDataSource(frame->provisionalDataSource());
  int http_status_code = response.httpStatusCode();

  // Record page load flags.
  WebURLResponseExtraDataImpl* extra_data =
      RenderViewImpl::GetExtraDataFromResponse(response);
  if (extra_data) {
    document_state->set_was_fetched_via_spdy(
        extra_data->was_fetched_via_spdy());
    document_state->set_was_npn_negotiated(
        extra_data->was_npn_negotiated());
    document_state->set_npn_negotiated_protocol(
        extra_data->npn_negotiated_protocol());
    document_state->set_was_alternate_protocol_available(
        extra_data->was_alternate_protocol_available());
    document_state->set_connection_info(
        extra_data->connection_info());
    document_state->set_was_fetched_via_proxy(
        extra_data->was_fetched_via_proxy());
  }
  InternalDocumentStateData* internal_data =
      InternalDocumentStateData::FromDocumentState(document_state);
  internal_data->set_http_status_code(http_status_code);
  // Whether or not the http status code actually corresponds to an error is
  // only checked when the page is done loading, if |use_error_page| is
  // still true.
  internal_data->set_use_error_page(true);
}

void RenderFrameImpl::didFinishResourceLoad(blink::WebFrame* frame,
                                            unsigned identifier) {
  // TODO(nasko): Move implementation here. Needed state:
  // * devtools_agent_
  // Needed methods:
  // * LoadNavigationErrorPage
  render_view_->didFinishResourceLoad(frame, identifier);
}

void RenderFrameImpl::didLoadResourceFromMemoryCache(
    blink::WebFrame* frame,
    const blink::WebURLRequest& request,
    const blink::WebURLResponse& response) {
  // The recipients of this message have no use for data: URLs: they don't
  // affect the page's insecure content list and are not in the disk cache. To
  // prevent large (1M+) data: URLs from crashing in the IPC system, we simply
  // filter them out here.
  GURL url(request.url());
  if (url.SchemeIs("data"))
    return;

  // Let the browser know we loaded a resource from the memory cache.  This
  // message is needed to display the correct SSL indicators.
  render_view_->Send(new ViewHostMsg_DidLoadResourceFromMemoryCache(
      render_view_->GetRoutingID(),
      url,
      response.securityInfo(),
      request.httpMethod().utf8(),
      response.mimeType().utf8(),
      ResourceType::FromTargetType(request.targetType())));
}

void RenderFrameImpl::didDisplayInsecureContent(blink::WebFrame* frame) {
  render_view_->Send(new ViewHostMsg_DidDisplayInsecureContent(
      render_view_->GetRoutingID()));
}

void RenderFrameImpl::didRunInsecureContent(
    blink::WebFrame* frame,
    const blink::WebSecurityOrigin& origin,
    const blink::WebURL& target) {
  render_view_->Send(new ViewHostMsg_DidRunInsecureContent(
      render_view_->GetRoutingID(),
      origin.toString().utf8(),
      target));
}

void RenderFrameImpl::didAbortLoading(blink::WebFrame* frame) {
#if defined(ENABLE_PLUGINS)
  if (frame != render_view_->webview()->mainFrame())
    return;
  PluginChannelHost::Broadcast(
      new PluginHostMsg_DidAbortLoading(render_view_->GetRoutingID()));
#endif
}

void RenderFrameImpl::didExhaustMemoryAvailableForScript(
    blink::WebFrame* frame) {
  render_view_->Send(new ViewHostMsg_JSOutOfMemory(
      render_view_->GetRoutingID()));
}

void RenderFrameImpl::didCreateScriptContext(blink::WebFrame* frame,
                                             v8::Handle<v8::Context> context,
                                             int extension_group,
                                             int world_id) {
  GetContentClient()->renderer()->DidCreateScriptContext(
      frame, context, extension_group, world_id);
}

void RenderFrameImpl::willReleaseScriptContext(blink::WebFrame* frame,
                                               v8::Handle<v8::Context> context,
                                               int world_id) {
  GetContentClient()->renderer()->WillReleaseScriptContext(
      frame, context, world_id);
}

void RenderFrameImpl::didFirstVisuallyNonEmptyLayout(blink::WebFrame* frame) {
  render_view_->didFirstVisuallyNonEmptyLayout(frame);
}

void RenderFrameImpl::didChangeContentsSize(blink::WebFrame* frame,
                                            const blink::WebSize& size) {
  // TODO(nasko): Move implementation here. Needed state:
  // * cached_has_main_frame_horizontal_scrollbar_
  // * cached_has_main_frame_vertical_scrollbar_
  render_view_->didChangeContentsSize(frame, size);
}

void RenderFrameImpl::didChangeScrollOffset(blink::WebFrame* frame) {
  // TODO(nasko): Move implementation here. Needed methods:
  // * StartNavStateSyncTimerIfNecessary
  render_view_->didChangeScrollOffset(frame);
}

void RenderFrameImpl::willInsertBody(blink::WebFrame* frame) {
  if (!frame->parent()) {
    render_view_->Send(new ViewHostMsg_WillInsertBody(
        render_view_->GetRoutingID()));
  }
}

void RenderFrameImpl::reportFindInPageMatchCount(int request_id,
                                                 int count,
                                                 bool final_update) {
  int active_match_ordinal = -1;  // -1 = don't update active match ordinal
  if (!count)
    active_match_ordinal = 0;

  render_view_->Send(new ViewHostMsg_Find_Reply(
      render_view_->GetRoutingID(), request_id, count,
      gfx::Rect(), active_match_ordinal, final_update));
}

void RenderFrameImpl::reportFindInPageSelection(
    int request_id,
    int active_match_ordinal,
    const blink::WebRect& selection_rect) {
  render_view_->Send(new ViewHostMsg_Find_Reply(
      render_view_->GetRoutingID(), request_id, -1, selection_rect,
      active_match_ordinal, false));
}

void RenderFrameImpl::requestStorageQuota(
    blink::WebFrame* frame,
    blink::WebStorageQuotaType type,
    unsigned long long requested_size,
    blink::WebStorageQuotaCallbacks* callbacks) {
  DCHECK(frame);
  WebSecurityOrigin origin = frame->document().securityOrigin();
  if (origin.isUnique()) {
    // Unique origins cannot store persistent state.
    callbacks->didFail(blink::WebStorageQuotaErrorAbort);
    return;
  }
  ChildThread::current()->quota_dispatcher()->RequestStorageQuota(
      render_view_->GetRoutingID(), GURL(origin.toString()),
      static_cast<quota::StorageType>(type), requested_size,
      QuotaDispatcher::CreateWebStorageQuotaCallbacksWrapper(callbacks));
}

void RenderFrameImpl::willOpenSocketStream(
    blink::WebSocketStreamHandle* handle) {
  SocketStreamHandleData::AddToHandle(handle, routing_id_);
}

void RenderFrameImpl::willStartUsingPeerConnectionHandler(
    blink::WebFrame* frame,
    blink::WebRTCPeerConnectionHandler* handler) {
#if defined(ENABLE_WEBRTC)
  static_cast<RTCPeerConnectionHandler*>(handler)->associateWithFrame(frame);
#endif
}

bool RenderFrameImpl::willCheckAndDispatchMessageEvent(
    blink::WebFrame* sourceFrame,
    blink::WebFrame* targetFrame,
    blink::WebSecurityOrigin targetOrigin,
    blink::WebDOMMessageEvent event) {
  // TODO(nasko): Move implementation here. Needed state:
  // * is_swapped_out_
  return render_view_->willCheckAndDispatchMessageEvent(
      sourceFrame, targetFrame, targetOrigin, event);
}

blink::WebString RenderFrameImpl::userAgentOverride(
    blink::WebFrame* frame,
    const blink::WebURL& url) {
  if (!render_view_->webview() || !render_view_->webview()->mainFrame() ||
      render_view_->renderer_preferences_.user_agent_override.empty()) {
    return blink::WebString();
  }

  // If we're in the middle of committing a load, the data source we need
  // will still be provisional.
  WebFrame* main_frame = render_view_->webview()->mainFrame();
  WebDataSource* data_source = NULL;
  if (main_frame->provisionalDataSource())
    data_source = main_frame->provisionalDataSource();
  else
    data_source = main_frame->dataSource();

  InternalDocumentStateData* internal_data = data_source ?
      InternalDocumentStateData::FromDataSource(data_source) : NULL;
  if (internal_data && internal_data->is_overriding_user_agent())
    return WebString::fromUTF8(
        render_view_->renderer_preferences_.user_agent_override);
  return blink::WebString();
}

blink::WebString RenderFrameImpl::doNotTrackValue(blink::WebFrame* frame) {
  if (render_view_->renderer_preferences_.enable_do_not_track)
    return WebString::fromUTF8("1");
  return WebString();
}

bool RenderFrameImpl::allowWebGL(blink::WebFrame* frame, bool default_value) {
  if (!default_value)
    return false;

  bool blocked = true;
  render_view_->Send(new ViewHostMsg_Are3DAPIsBlocked(
      render_view_->GetRoutingID(),
      GURL(frame->top()->document().securityOrigin().toString()),
      THREE_D_API_TYPE_WEBGL,
      &blocked));
  return !blocked;
}

void RenderFrameImpl::didLoseWebGLContext(blink::WebFrame* frame,
                                          int arb_robustness_status_code) {
  render_view_->Send(new ViewHostMsg_DidLose3DContext(
      GURL(frame->top()->document().securityOrigin().toString()),
      THREE_D_API_TYPE_WEBGL,
      arb_robustness_status_code));
}

void RenderFrameImpl::AddObserver(RenderFrameObserver* observer) {
  observers_.AddObserver(observer);
}

void RenderFrameImpl::RemoveObserver(RenderFrameObserver* observer) {
  observer->RenderFrameGone();
  observers_.RemoveObserver(observer);
}

}  // namespace content
