// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef CHROME_BROWSER_SIGNIN_ACCOUNT_RECONCILOR_H_
#define CHROME_BROWSER_SIGNIN_ACCOUNT_RECONCILOR_H_

#include <deque>

#include "base/basictypes.h"
#include "base/callback_forward.h"
#include "base/compiler_specific.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/scoped_vector.h"
#include "chrome/browser/signin/signin_manager.h"
#include "components/browser_context_keyed_service/browser_context_keyed_service.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "google_apis/gaia/gaia_auth_consumer.h"
#include "google_apis/gaia/google_service_auth_error.h"
#include "google_apis/gaia/merge_session_helper.h"
#include "google_apis/gaia/oauth2_token_service.h"

class GaiaAuthFetcher;
class Profile;
struct ChromeCookieDetails;

class AccountReconcilor : public BrowserContextKeyedService,
                          public content::NotificationObserver,
                          public GaiaAuthConsumer,
                          public OAuth2TokenService::Consumer,
                          public OAuth2TokenService::Observer,
                          public SigninManagerBase::Observer {
 public:
  explicit AccountReconcilor(Profile* profile);
  virtual ~AccountReconcilor();

  // BrowserContextKeyedService implementation.
  virtual void Shutdown() OVERRIDE;

  // Add or remove observers for the merge session notification.
  void AddMergeSessionObserver(MergeSessionHelper::Observer* observer);
  void RemoveMergeSessionObserver(MergeSessionHelper::Observer* observer);

  Profile* profile() { return profile_; }

  bool IsPeriodicReconciliationRunning() const {
    return reconciliation_timer_.IsRunning();
  }

  bool IsRegisteredWithTokenService() const {
    return registered_with_token_service_;
  }

  bool AreGaiaAccountsSet() const { return are_gaia_accounts_set_; }

  bool AreAllRefreshTokensChecked() const;

  const std::vector<std::string>& GetGaiaAccountsForTesting() const {
    return gaia_accounts_;
  }

  const std::set<std::string>& GetValidChromeAccountsForTesting() const {
    return valid_chrome_accounts_;
  }

  const std::set<std::string>& GetInvalidChromeAccountsForTesting() const {
    return invalid_chrome_accounts_;
  }

 private:
  // Used during GetAccountsFromCookie.
  // Stores a callback for the next action to perform.
  typedef base::Callback<void(
      const GoogleServiceAuthError& error,
      const std::vector<std::string>&)> GetAccountsFromCookieCallback;

  class AccountReconcilorTest;
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest, GetAccountsFromCookieSuccess);
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest, GetAccountsFromCookieFailure);
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest, ValidateAccountsFromTokens);
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest,
                           ValidateAccountsFromTokensFailedUserInfo);
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest,
                            ValidateAccountsFromTokensFailedTokenRequest);
  FRIEND_TEST_ALL_PREFIXES(AccountReconcilorTest, StartReconcileAction);

  class UserIdFetcher;

  // Register and unregister with dependent services.
  void RegisterWithCookieMonster();
  void UnregisterWithCookieMonster();
  void RegisterWithSigninManager();
  void UnregisterWithSigninManager();
  void RegisterWithTokenService();
  void UnregisterWithTokenService();

  bool IsProfileConnected();

  void DeleteAccessTokenRequestsAndUserIdFetchers();

  // Start and stop the periodic reconciliation.
  void StartPeriodicReconciliation();
  void StopPeriodicReconciliation();
  void PeriodicReconciliation();

  void PerformMergeAction(const std::string& account_id);
  void StartRemoveAction(const std::string& account_id);
  void FinishRemoveAction(
      const std::string& account_id,
      const GoogleServiceAuthError& error,
      const std::vector<std::string>& accounts);

  // Used during period reconciliation.
  void StartReconcileAction();
  void FinishReconcileAction();
  void HandleSuccessfulAccountIdCheck(const std::string& account_id);
  void HandleFailedAccountIdCheck(const std::string& account_id);

  void GetAccountsFromCookie(GetAccountsFromCookieCallback callback);
  void ContinueReconcileActionAfterGetGaiaAccounts(
      const GoogleServiceAuthError& error,
      const std::vector<std::string>& accounts);
  void ValidateAccountsFromTokenService();

  void OnCookieChanged(ChromeCookieDetails* details);

  // Overriden from content::NotificationObserver.
  virtual void Observe(int type,
                       const content::NotificationSource& source,
                       const content::NotificationDetails& details) OVERRIDE;

  // Overriden from OAuth2TokenService::Consumer.
  virtual void OnGetTokenSuccess(const OAuth2TokenService::Request* request,
                                 const std::string& access_token,
                                 const base::Time& expiration_time) OVERRIDE;
  virtual void OnGetTokenFailure(const OAuth2TokenService::Request* request,
                                 const GoogleServiceAuthError& error) OVERRIDE;

  // Overriden from OAuth2TokenService::Observer.
  virtual void OnRefreshTokenAvailable(const std::string& account_id) OVERRIDE;
  virtual void OnRefreshTokenRevoked(const std::string& account_id) OVERRIDE;
  virtual void OnRefreshTokensLoaded() OVERRIDE;

  // Overriden from SigninManagerBase::Observer.
  virtual void GoogleSigninSucceeded(const std::string& username,
                                     const std::string& password) OVERRIDE;
  virtual void GoogleSignedOut(const std::string& username) OVERRIDE;

  // Overriden from GaiaAuthConsumer.
  virtual void OnListAccountsSuccess(const std::string& data) OVERRIDE;
  virtual void OnListAccountsFailure(
      const GoogleServiceAuthError& error) OVERRIDE;

  void MayBeDoNextListAccounts();

  // The profile that this reconcilor belongs to.
  Profile* profile_;
  content::NotificationRegistrar registrar_;
  base::RepeatingTimer<AccountReconcilor> reconciliation_timer_;
  MergeSessionHelper merge_session_helper_;
  bool registered_with_token_service_;

  // Used during reconcile action.
  // These members are used used to validate the gaia cookie.
  scoped_ptr<GaiaAuthFetcher> gaia_fetcher_;
  bool are_gaia_accounts_set_;
  std::vector<std::string> gaia_accounts_;

  // Used during reconcile action.
  // These members are used to validate the tokens in OAuth2TokenService.
  std::string primary_account_;
  std::vector<std::string> chrome_accounts_;
  scoped_ptr<OAuth2TokenService::Request>* requests_;
  ScopedVector<UserIdFetcher> user_id_fetchers_;
  std::set<std::string> valid_chrome_accounts_;
  std::set<std::string> invalid_chrome_accounts_;

  std::deque<GetAccountsFromCookieCallback> get_gaia_accounts_callbacks_;

  DISALLOW_COPY_AND_ASSIGN(AccountReconcilor);
};

#endif  // CHROME_BROWSER_SIGNIN_ACCOUNT_RECONCILOR_H_
