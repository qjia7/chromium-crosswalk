// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/shell/shell_main_delegate.h"

#include "apps/shell/shell_content_browser_client.h"
#include "apps/shell/shell_content_client.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "content/public/browser/browser_main_runner.h"
#include "ui/base/resource/resource_bundle.h"

namespace apps {

ShellMainDelegate::ShellMainDelegate() {
}

ShellMainDelegate::~ShellMainDelegate() {
}

bool ShellMainDelegate::BasicStartupComplete(int* exit_code) {
  // TODO(jamescook): Initialize logging here.
  content_client_.reset(new ShellContentClient);
  SetContentClient(content_client_.get());
  return false;
}

void ShellMainDelegate::PreSandboxStartup() {
  InitializeResourceBundle();
}

content::ContentBrowserClient* ShellMainDelegate::CreateContentBrowserClient() {
  browser_client_.reset(new apps::ShellContentBrowserClient);
  return browser_client_.get();
}

content::ContentRendererClient*
ShellMainDelegate::CreateContentRendererClient() {
  // TODO(jamescook): Create a ShellContentRendererClient with the extensions
  // initialization pieces of ChromeContentRendererClient.
  return content::ContentMainDelegate::CreateContentRendererClient();
}

void ShellMainDelegate::InitializeResourceBundle() {
  ui::ResourceBundle::InitSharedInstanceWithLocale("en-US", NULL);
}

}  // namespace apps
