// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_BROWSER_API_USB_USB_API_H_
#define EXTENSIONS_BROWSER_API_USB_USB_API_H_

#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_ptr.h"
#include "device/usb/usb_device.h"
#include "device/usb/usb_device_filter.h"
#include "device/usb/usb_device_handle.h"
#include "extensions/browser/api/api_resource_manager.h"
#include "extensions/browser/api/async_api_function.h"
#include "extensions/browser/api/device_permissions_prompt.h"
#include "extensions/common/api/usb.h"
#include "net/base/io_buffer.h"

namespace extensions {

class DevicePermissions;
class UsbDeviceResource;

class UsbAsyncApiFunction : public AsyncApiFunction {
 public:
  UsbAsyncApiFunction();

 protected:
  virtual ~UsbAsyncApiFunction();

  virtual bool PrePrepare() override;
  virtual bool Respond() override;

  bool HasDevicePermission(scoped_refptr<device::UsbDevice> device);
  scoped_refptr<device::UsbDevice> GetDeviceOrCompleteWithError(
      const extensions::core_api::usb::Device& input_device);
  scoped_refptr<device::UsbDeviceHandle> GetDeviceHandleOrCompleteWithError(
      const extensions::core_api::usb::ConnectionHandle& input_device_handle);

  void RemoveUsbDeviceResource(int api_resource_id);

  void CompleteWithError(const std::string& error);

  ApiResourceManager<UsbDeviceResource>* manager_;
  scoped_ptr<DevicePermissions> device_permissions_;
};

class UsbAsyncApiTransferFunction : public UsbAsyncApiFunction {
 protected:
  UsbAsyncApiTransferFunction();
  virtual ~UsbAsyncApiTransferFunction();

  bool ConvertDirectionSafely(const extensions::core_api::usb::Direction& input,
                              device::UsbEndpointDirection* output);
  bool ConvertRequestTypeSafely(
      const extensions::core_api::usb::RequestType& input,
      device::UsbDeviceHandle::TransferRequestType* output);
  bool ConvertRecipientSafely(
      const extensions::core_api::usb::Recipient& input,
      device::UsbDeviceHandle::TransferRecipient* output);

  void OnCompleted(device::UsbTransferStatus status,
                   scoped_refptr<net::IOBuffer> data,
                   size_t length);
};

class UsbFindDevicesFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.findDevices", USB_FINDDEVICES)

  UsbFindDevicesFunction();

 protected:
  virtual ~UsbFindDevicesFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  void OpenDevices(
      scoped_ptr<std::vector<scoped_refptr<device::UsbDevice> > > devices);

  std::vector<scoped_refptr<device::UsbDeviceHandle> > device_handles_;
  scoped_ptr<extensions::core_api::usb::FindDevices::Params> parameters_;
};

class UsbGetDevicesFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.getDevices", USB_GETDEVICES)

  UsbGetDevicesFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 protected:
  virtual ~UsbGetDevicesFunction();

 private:
  scoped_ptr<extensions::core_api::usb::GetDevices::Params> parameters_;
};

class UsbGetUserSelectedDevicesFunction
    : public UIThreadExtensionFunction,
      public DevicePermissionsPrompt::Delegate {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.getUserSelectedDevices",
                             USB_GETUSERSELECTEDDEVICES)

  UsbGetUserSelectedDevicesFunction();

 protected:
  virtual ~UsbGetUserSelectedDevicesFunction();
  virtual ResponseAction Run() override;

 private:
  virtual void OnUsbDevicesChosen(
      const std::vector<scoped_refptr<device::UsbDevice>>& devices) override;

  scoped_ptr<DevicePermissionsPrompt> prompt_;
  std::vector<uint32> device_ids_;
  std::vector<scoped_refptr<device::UsbDevice>> devices_;
  std::vector<base::string16> serial_numbers_;
};

class UsbRequestAccessFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.requestAccess", USB_REQUESTACCESS)

  UsbRequestAccessFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 protected:
  virtual ~UsbRequestAccessFunction();

  void OnCompleted(bool success);

 private:
  scoped_ptr<extensions::core_api::usb::RequestAccess::Params> parameters_;
};

class UsbOpenDeviceFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.openDevice", USB_OPENDEVICE)

  UsbOpenDeviceFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 protected:
  virtual ~UsbOpenDeviceFunction();

 private:
  scoped_refptr<device::UsbDeviceHandle> handle_;
  scoped_ptr<extensions::core_api::usb::OpenDevice::Params> parameters_;
};

class UsbGetConfigurationFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.getConfiguration", USB_GETCONFIGURATION)

  UsbGetConfigurationFunction();

 protected:
  virtual ~UsbGetConfigurationFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::GetConfiguration::Params> parameters_;
};

class UsbListInterfacesFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.listInterfaces", USB_LISTINTERFACES)

  UsbListInterfacesFunction();

 protected:
  virtual ~UsbListInterfacesFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::ListInterfaces::Params> parameters_;
};

class UsbCloseDeviceFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.closeDevice", USB_CLOSEDEVICE)

  UsbCloseDeviceFunction();

 protected:
  virtual ~UsbCloseDeviceFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::CloseDevice::Params> parameters_;
};

class UsbClaimInterfaceFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.claimInterface", USB_CLAIMINTERFACE)

  UsbClaimInterfaceFunction();

 protected:
  virtual ~UsbClaimInterfaceFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::ClaimInterface::Params> parameters_;
};

class UsbReleaseInterfaceFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.releaseInterface", USB_RELEASEINTERFACE)

  UsbReleaseInterfaceFunction();

 protected:
  virtual ~UsbReleaseInterfaceFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::ReleaseInterface::Params> parameters_;
};

class UsbSetInterfaceAlternateSettingFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.setInterfaceAlternateSetting",
                             USB_SETINTERFACEALTERNATESETTING)

  UsbSetInterfaceAlternateSettingFunction();

 private:
  virtual ~UsbSetInterfaceAlternateSettingFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

  scoped_ptr<extensions::core_api::usb::SetInterfaceAlternateSetting::Params>
      parameters_;
};

class UsbControlTransferFunction : public UsbAsyncApiTransferFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.controlTransfer", USB_CONTROLTRANSFER)

  UsbControlTransferFunction();

 protected:
  virtual ~UsbControlTransferFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::ControlTransfer::Params> parameters_;
};

class UsbBulkTransferFunction : public UsbAsyncApiTransferFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.bulkTransfer", USB_BULKTRANSFER)

  UsbBulkTransferFunction();

 protected:
  virtual ~UsbBulkTransferFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::BulkTransfer::Params> parameters_;
};

class UsbInterruptTransferFunction : public UsbAsyncApiTransferFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.interruptTransfer", USB_INTERRUPTTRANSFER)

  UsbInterruptTransferFunction();

 protected:
  virtual ~UsbInterruptTransferFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::InterruptTransfer::Params> parameters_;
};

class UsbIsochronousTransferFunction : public UsbAsyncApiTransferFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.isochronousTransfer", USB_ISOCHRONOUSTRANSFER)

  UsbIsochronousTransferFunction();

 protected:
  virtual ~UsbIsochronousTransferFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::IsochronousTransfer::Params>
      parameters_;
};

class UsbResetDeviceFunction : public UsbAsyncApiFunction {
 public:
  DECLARE_EXTENSION_FUNCTION("usb.resetDevice", USB_RESETDEVICE)

  UsbResetDeviceFunction();

 protected:
  virtual ~UsbResetDeviceFunction();

  virtual bool Prepare() override;
  virtual void AsyncWorkStart() override;

 private:
  scoped_ptr<extensions::core_api::usb::ResetDevice::Params> parameters_;
};
}  // namespace extensions

#endif  // EXTENSIONS_BROWSER_API_USB_USB_API_H_
