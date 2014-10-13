// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_BROWSER_GUEST_VIEW_MIME_HANDLER_VIEW_MIME_HANDLER_VIEW_GUEST_H_
#define EXTENSIONS_BROWSER_GUEST_VIEW_MIME_HANDLER_VIEW_MIME_HANDLER_VIEW_GUEST_H_

#include "extensions/browser/extension_function_dispatcher.h"
#include "extensions/browser/guest_view/guest_view.h"

namespace content {
class WebContents;
}  // namespace content

namespace extensions {

class MimeHandlerViewGuestDelegate;

class MimeHandlerViewGuest : public GuestView<MimeHandlerViewGuest>,
                             public ExtensionFunctionDispatcher::Delegate {
 public:
  static GuestViewBase* Create(content::BrowserContext* browser_context,
                               int guest_instance_id);

  static const char Type[];

  // ExtensionFunctionDispatcher::Delegate implementation.
  virtual WindowController* GetExtensionWindowController() const override;
  virtual content::WebContents* GetAssociatedWebContents() const override;

  // GuestViewBase implementation.
  virtual const char* GetAPINamespace() const override;
  virtual int GetTaskPrefix() const override;
  virtual void CreateWebContents(
      const std::string& embedder_extension_id,
      int embedder_render_process_id,
      const GURL& embedder_site_url,
      const base::DictionaryValue& create_params,
      const WebContentsCreatedCallback& callback) override;
  virtual void DidAttachToEmbedder() override;
  virtual void DidInitialize() override;

  // content::BrowserPluginGuestDelegate implementation
  virtual bool Find(int request_id,
                    const base::string16& search_text,
                    const blink::WebFindOptions& options,
                    bool is_full_page_plugin) override;

  // WebContentsDelegate implementation.
  virtual void ContentsZoomChange(bool zoom_in) override;
  virtual void HandleKeyboardEvent(
      content::WebContents* source,
      const content::NativeWebKeyboardEvent& event) override;
  virtual void FindReply(content::WebContents* web_contents,
                         int request_id,
                         int number_of_matches,
                         const gfx::Rect& selection_rect,
                         int active_match_ordinal,
                         bool final_update) override;

  // content::WebContentsObserver implementation.
  virtual bool OnMessageReceived(const IPC::Message& message) override;

 private:
  MimeHandlerViewGuest(content::BrowserContext* browser_context,
                       int guest_instance_id);
  virtual ~MimeHandlerViewGuest();

  void OnRequest(const ExtensionHostMsg_Request_Params& params);

  scoped_ptr<MimeHandlerViewGuestDelegate> delegate_;
  scoped_ptr<ExtensionFunctionDispatcher> extension_function_dispatcher_;

  DISALLOW_COPY_AND_ASSIGN(MimeHandlerViewGuest);
};

}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_GUEST_VIEW_MIME_HANDLER_VIEW_MIME_HANDLER_VIEW_GUEST_H_
