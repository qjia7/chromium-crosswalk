// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/apps/app_browsertest_util.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_system.h"
#include "chrome/browser/extensions/extension_test_message_listener.h"
#include "chrome/common/extensions/api/alarms.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/process_manager.h"
#include "extensions/common/switches.h"

using extensions::Event;
using extensions::EventRouter;
using extensions::Extension;
using extensions::ExtensionSystem;
using extensions::PlatformAppBrowserTest;

namespace {

namespace alarms = extensions::api::alarms;

const char kDispatchEventTestApp[] =
    "platform_apps/ephemeral_apps/dispatch_event";

const char kMessagingReceiverApp[] =
    "platform_apps/ephemeral_apps/messaging_receiver";

const char kMessagingReceiverAppV2[] =
    "platform_apps/ephemeral_apps/messaging_receiver2";

class EphemeralAppBrowserTest : public PlatformAppBrowserTest {
 protected:
  virtual void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    // Skip PlatformAppBrowserTest, which sets different values for the switches
    // below.
    ExtensionBrowserTest::SetUpCommandLine(command_line);

    // Make event pages get suspended immediately.
    command_line->AppendSwitchASCII(
        extensions::switches::kEventPageIdleTime, "10");
    command_line->AppendSwitchASCII(
        extensions::switches::kEventPageSuspendingTime, "10");
  }

  const Extension* InstallEphemeralApp(const char* test_path) {
    base::FilePath path = test_data_dir_.AppendASCII(test_path);
    const Extension* extension =
        InstallExtensionWithSourceAndFlags(
            path,
            1,
            extensions::Manifest::UNPACKED,
            Extension::IS_EPHEMERAL);
    return extension;
  }

  const Extension* InstallAndLaunchEphemeralApp(const char* test_path) {
    ExtensionTestMessageListener launched_listener("launched", false);
    const Extension* extension = InstallEphemeralApp(test_path);
    EXPECT_TRUE(extension);
    if (!extension)
      return NULL;

    LaunchPlatformApp(extension);
    bool wait_result = launched_listener.WaitUntilSatisfied();
    EXPECT_TRUE(wait_result);
    if (!wait_result)
      return NULL;

    return extension;
  }

  void CloseApp(const std::string& app_id) {
    content::WindowedNotificationObserver event_page_destroyed_signal(
        chrome::NOTIFICATION_EXTENSION_HOST_DESTROYED,
        content::Source<Profile>(browser()->profile()));

    EXPECT_EQ(1U, GetShellWindowCountForApp(app_id));
    apps::ShellWindow* shell_window = GetFirstShellWindowForApp(app_id);
    ASSERT_TRUE(shell_window);
    CloseShellWindow(shell_window);

    event_page_destroyed_signal.Wait();
  }

  void VerifyAppNotLoaded(const std::string& app_id) {
    EXPECT_FALSE(ExtensionSystem::Get(browser()->profile())->
        process_manager()->GetBackgroundHostForExtension(app_id));
  }

  void DispatchAlarmEvent(EventRouter* event_router,
                          const std::string& app_id) {
    alarms::Alarm dummy_alarm;
    dummy_alarm.name = "test_alarm";

    scoped_ptr<base::ListValue> args(new base::ListValue());
    args->Append(dummy_alarm.ToValue().release());
    scoped_ptr<Event> event(new Event(alarms::OnAlarm::kEventName,
                                      args.Pass()));

    event_router->DispatchEventToExtension(app_id, event.Pass());
  }
};

}  // namespace

// Verify that ephemeral apps can be launched and receive system events when
// they are running. Once they are inactive they should not receive system
// events.
IN_PROC_BROWSER_TEST_F(EphemeralAppBrowserTest, EventDispatchWhenLaunched) {
  const Extension* extension =
      InstallAndLaunchEphemeralApp(kDispatchEventTestApp);
  ASSERT_TRUE(extension);

  // Send a fake alarm event to the app and verify that a response is
  // received.
  EventRouter* event_router =
      ExtensionSystem::Get(browser()->profile())->event_router();
  ASSERT_TRUE(event_router);

  ExtensionTestMessageListener alarm_received_listener("alarm_received", false);
  DispatchAlarmEvent(event_router, extension->id());
  ASSERT_TRUE(alarm_received_listener.WaitUntilSatisfied());

  CloseApp(extension->id());

  // The app needs to be launched once in order to have the onAlarm() event
  // registered.
  ASSERT_TRUE(event_router->ExtensionHasEventListener(
      extension->id(), alarms::OnAlarm::kEventName));

  // Dispatch the alarm event again and verify that the event page did not get
  // loaded for the app.
  DispatchAlarmEvent(event_router, extension->id());
  VerifyAppNotLoaded(extension->id());
}

// Verify that ephemeral apps will receive messages while they are running.
IN_PROC_BROWSER_TEST_F(EphemeralAppBrowserTest, ReceiveMessagesWhenLaunched) {
  const Extension* receiver =
      InstallAndLaunchEphemeralApp(kMessagingReceiverApp);
  ASSERT_TRUE(receiver);

  // Verify that messages are received while the app is running.
  ExtensionApiTest::ResultCatcher result_catcher;
  LoadAndLaunchPlatformApp("ephemeral_apps/messaging_sender_success");
  EXPECT_TRUE(result_catcher.GetNextResult());

  CloseApp(receiver->id());

  // Verify that messages are not received while the app is inactive.
  LoadAndLaunchPlatformApp("ephemeral_apps/messaging_sender_fail");
  EXPECT_TRUE(result_catcher.GetNextResult());
}

// Verify that an updated ephemeral app will still have its ephemeral flag
// enabled.
IN_PROC_BROWSER_TEST_F(EphemeralAppBrowserTest, UpdateEphemeralApp) {
  const Extension* app_v1 = InstallEphemeralApp(kMessagingReceiverApp);
  ASSERT_TRUE(app_v1);
  ASSERT_TRUE(app_v1->is_ephemeral());
  std::string app_id = app_v1->id();
  base::Version app_original_version = *app_v1->version();
  app_v1 = NULL; // The extension object will be destroyed during update.

  // Pack version 2 of the app.
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  base::FilePath crx_path = temp_dir.path().AppendASCII("temp.crx");
  if (!base::DeleteFile(crx_path, false)) {
    ADD_FAILURE() << "Failed to delete crx: " << crx_path.value();
    return;
  }

  base::FilePath app_v2_path = PackExtensionWithOptions(
      test_data_dir_.AppendASCII(kMessagingReceiverAppV2),
      crx_path,
      test_data_dir_.AppendASCII(kMessagingReceiverApp).ReplaceExtension(
          FILE_PATH_LITERAL(".pem")),
      base::FilePath());
  ASSERT_FALSE(app_v2_path.empty());

  // Update the ephemeral app and wait for the update to finish.
  extensions::CrxInstaller* crx_installer = NULL;
  content::WindowedNotificationObserver windowed_observer(
      chrome::NOTIFICATION_CRX_INSTALLER_DONE,
      content::Source<extensions::CrxInstaller>(crx_installer));
  ExtensionService* service =
      ExtensionSystem::Get(browser()->profile())->extension_service();
  EXPECT_TRUE(service->UpdateExtension(app_id, app_v2_path, true, GURL(),
                                       &crx_installer));
  windowed_observer.Wait();

  const Extension* app_v2 = service->GetExtensionById(app_id, false);
  ASSERT_TRUE(app_v2);
  EXPECT_TRUE(app_v2->version()->CompareTo(app_original_version) > 0);
  EXPECT_TRUE(app_v2->is_ephemeral());
}
