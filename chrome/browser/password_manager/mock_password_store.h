// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PASSWORD_MANAGER_MOCK_PASSWORD_STORE_H_
#define CHROME_BROWSER_PASSWORD_MANAGER_MOCK_PASSWORD_STORE_H_

#include "chrome/browser/password_manager/password_store.h"
#include "components/autofill/core/common/password_form.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace content {
class BrowserContext;
}

class MockPasswordStore : public PasswordStore {
 public:
  MockPasswordStore();

  static scoped_refptr<RefcountedBrowserContextKeyedService> Build(
      content::BrowserContext* profile);

  MOCK_METHOD1(RemoveLogin, void(const autofill::PasswordForm&));
  MOCK_METHOD3(GetLogins, void(
      const autofill::PasswordForm&,
      PasswordStore::AuthorizationPromptPolicy prompt_policy,
      PasswordStoreConsumer*));
  MOCK_METHOD1(AddLogin, void(const autofill::PasswordForm&));
  MOCK_METHOD1(UpdateLogin, void(const autofill::PasswordForm&));
  MOCK_METHOD0(ReportMetrics, void());
  MOCK_METHOD0(ReportMetricsImpl, void());
  MOCK_METHOD1(AddLoginImpl, void(const autofill::PasswordForm&));
  MOCK_METHOD1(UpdateLoginImpl, void(const autofill::PasswordForm&));
  MOCK_METHOD1(RemoveLoginImpl, void(const autofill::PasswordForm&));
  MOCK_METHOD2(RemoveLoginsCreatedBetweenImpl,
               void(const base::Time&, const base::Time&));
  MOCK_METHOD3(GetLoginsImpl,
               void(const autofill::PasswordForm& form,
                    PasswordStore::AuthorizationPromptPolicy prompt_policy,
                    const ConsumerCallbackRunner& callback_runner));
  MOCK_METHOD1(GetAutofillableLoginsImpl, void(GetLoginsRequest*));
  MOCK_METHOD1(GetBlacklistLoginsImpl, void(GetLoginsRequest*));
  MOCK_METHOD1(FillAutofillableLogins,
      bool(std::vector<autofill::PasswordForm*>*));
  MOCK_METHOD1(FillBlacklistLogins,
      bool(std::vector<autofill::PasswordForm*>*));

  virtual void ShutdownOnUIThread();

 protected:
  virtual ~MockPasswordStore();
};

#endif  // CHROME_BROWSER_PASSWORD_MANAGER_MOCK_PASSWORD_STORE_H_
