// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/declarative_content/content_constants.h"

namespace extensions {
namespace declarative_content_constants {

// Signals to which ContentRulesRegistries are registered.
const char kOnPageChanged[] = "declarativeContent.onPageChanged";

// Keys of dictionaries.
const char kCss[] = "css";
const char kInstanceType[] = "instanceType";
const char kPageUrl[] = "pageUrl";

// Values of dictionaries, in particular instance types
const char kPageStateMatcherType[] = "declarativeContent.PageStateMatcher";
const char kShowPageAction[] = "declarativeContent.ShowPageAction";

}  // namespace declarative_content_constants
}  // namespace extensions
