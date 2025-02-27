// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "chrome/browser/bluetooth/bluetooth_chooser_context.h"
#include "chrome/browser/bluetooth/bluetooth_chooser_context_factory.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/permissions/chooser_context_base.h"
#include "chrome/browser/permissions/chooser_context_base_mock_permission_observer.h"
#include "chrome/test/base/testing_profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "content/public/test/browser_task_environment.h"
#include "device/bluetooth/public/cpp/bluetooth_uuid.h"
#include "device/bluetooth/test/mock_bluetooth_adapter.h"
#include "device/bluetooth/test/mock_bluetooth_device.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/bluetooth/web_bluetooth.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

using blink::mojom::WebBluetoothRequestDeviceOptionsPtr;
using device::BluetoothUUID;
using device::BluetoothUUIDHash;

namespace {

constexpr char kDeviceAddressKey[] = "device-address";
constexpr char kDeviceNameKey[] = "name";
constexpr char kServicesKey[] = "services";
constexpr char kWebBluetoothDeviceIdKey[] = "web-bluetooth-device-id";

const uint32_t kGamepadBluetoothClass = 0x0508;

constexpr char kDeviceAddress1[] = "00:00:00:00:00:00";
constexpr char kDeviceAddress2[] = "11:11:11:11:11:11";

constexpr char kGlucoseUUIDString[] = "00001808-0000-1000-8000-00805f9b34fb";
constexpr char kHeartRateUUIDString[] = "0000180d-0000-1000-8000-00805f9b34fb";
constexpr char kBatteryServiceUUIDString[] =
    "0000180f-0000-1000-8000-00805f9b34fb";
constexpr char kBloodPressureUUIDString[] =
    "00001813-0000-1000-8000-00805f9b34fb";
constexpr char kCyclingPowerUUIDString[] =
    "00001818-0000-1000-8000-00805f9b34fb";
const BluetoothUUID kGlucoseUUID(kGlucoseUUIDString);
const BluetoothUUID kHeartRateUUID(kHeartRateUUIDString);
const BluetoothUUID kBatteryServiceUUID(kBatteryServiceUUIDString);
const BluetoothUUID kBloodPressureUUID(kBloodPressureUUIDString);
const BluetoothUUID kCyclingPowerUUID(kCyclingPowerUUIDString);

WebBluetoothRequestDeviceOptionsPtr CreateOptionsForServices(
    const std::vector<BluetoothUUID>& filter_services,
    const std::vector<BluetoothUUID>& optional_services) {
  auto filter = blink::mojom::WebBluetoothLeScanFilter::New();
  filter->services = filter_services;

  std::vector<blink::mojom::WebBluetoothLeScanFilterPtr> scan_filters;
  scan_filters.push_back(std::move(filter));

  auto options = blink::mojom::WebBluetoothRequestDeviceOptions::New();
  options->filters = std::move(scan_filters);
  options->optional_services = optional_services;
  return options;
}

WebBluetoothRequestDeviceOptionsPtr CreateOptionsForServices(
    const std::vector<BluetoothUUID>& filter_services) {
  return CreateOptionsForServices(filter_services, {});
}

}  // namespace

class FakeBluetoothAdapter : public device::MockBluetoothAdapter {
 public:
  FakeBluetoothAdapter() = default;

  // Move-only class.
  FakeBluetoothAdapter(const FakeBluetoothAdapter&) = delete;
  FakeBluetoothAdapter& operator=(const FakeBluetoothAdapter&) = delete;

 private:
  ~FakeBluetoothAdapter() override = default;
};

class FakeBluetoothDevice : public device::MockBluetoothDevice {
 public:
  FakeBluetoothDevice(device::MockBluetoothAdapter* adapter,
                      const char* name,
                      const std::string& address)
      : device::MockBluetoothDevice(adapter,
                                    kGamepadBluetoothClass,
                                    name,
                                    address,
                                    /*paired=*/true,
                                    /*connected=*/true) {}

  // Move-only class.
  FakeBluetoothDevice(const FakeBluetoothDevice&) = delete;
  FakeBluetoothDevice& operator=(const FakeBluetoothDevice&) = delete;
};

class BluetoothChooserContextTest : public testing::Test {
 public:
  BluetoothChooserContextTest()
      : foo_url_("https://foo.com"),
        bar_url_("https://bar.com"),
        foo_origin_(url::Origin::Create(foo_url_)),
        bar_origin_(url::Origin::Create(bar_url_)) {}

  ~BluetoothChooserContextTest() override = default;

  // Move-only class.
  BluetoothChooserContextTest(const BluetoothChooserContextTest&) = delete;
  BluetoothChooserContextTest& operator=(const BluetoothChooserContextTest&) =
      delete;

  void SetUp() override {
    fake_adapter_ = base::MakeRefCounted<FakeBluetoothAdapter>();
    fake_device1_ = GetBluetoothDevice("Wireless Gizmo", kDeviceAddress1);
    fake_device2_ = GetBluetoothDevice("Wireless Gadget", kDeviceAddress2);
  }

 protected:
  Profile* profile() { return &profile_; }

  BluetoothChooserContext* GetChooserContext(Profile* profile) {
    auto* chooser_context =
        BluetoothChooserContextFactory::GetForProfile(profile);
    chooser_context->AddObserver(&mock_permission_observer_);
    return chooser_context;
  }

  std::unique_ptr<FakeBluetoothDevice> GetBluetoothDevice(const char* name,
                                                          std::string address) {
    return std::make_unique<FakeBluetoothDevice>(fake_adapter_.get(), name,
                                                 address);
  }

  // Mock Observer
  MockPermissionObserver mock_permission_observer_;

  const GURL foo_url_;
  const GURL bar_url_;
  const url::Origin foo_origin_;
  const url::Origin bar_origin_;
  std::unique_ptr<FakeBluetoothDevice> fake_device1_;
  std::unique_ptr<FakeBluetoothDevice> fake_device2_;

 private:
  content::BrowserTaskEnvironment task_environment_;
  scoped_refptr<FakeBluetoothAdapter> fake_adapter_;
  TestingProfile profile_;
};

// Check that Web Bluetooth device permissions are granted and revoked properly,
// and that the WebBluetoothDeviceId and device address can be retrieved using
// each other.
TEST_F(BluetoothChooserContextTest, CheckGrantAndRevokePermission) {
  const std::vector<BluetoothUUID> services = {kGlucoseUUID,
                                               kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options =
      CreateOptionsForServices(services);

  BluetoothChooserContext* context = GetChooserContext(profile());

  EXPECT_FALSE(context
                   ->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress())
                   .IsValid());
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));

  blink::WebBluetoothDeviceId device_id = context->GrantServiceAccessPermission(
      foo_origin_, foo_origin_, fake_device1_.get(), options);

  EXPECT_TRUE(
      context->HasDevicePermission(foo_origin_, foo_origin_, device_id));
  EXPECT_EQ(context->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress()),
            device_id);
  EXPECT_EQ(context->GetDeviceAddress(foo_origin_, foo_origin_, device_id),
            fake_device1_->GetAddress());
  EXPECT_TRUE(context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, device_id));
  for (const auto& service : services) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id, service));
  }

  base::Value expected_object(base::Value::Type::DICTIONARY);
  expected_object.SetStringKey(kDeviceAddressKey, kDeviceAddress1);
  expected_object.SetStringKey(kDeviceNameKey,
                               fake_device1_->GetNameForDisplay());
  expected_object.SetStringKey(kWebBluetoothDeviceIdKey, device_id.str());
  base::Value expected_services(base::Value::Type::DICTIONARY);
  expected_services.SetBoolKey(kGlucoseUUIDString, /*val=*/true);
  expected_services.SetBoolKey(kBloodPressureUUIDString, /*val=*/true);
  expected_object.SetKey(kServicesKey, std::move(expected_services));

  std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
      context->GetGrantedObjects(foo_origin_, foo_origin_);
  ASSERT_EQ(1u, origin_objects.size());
  EXPECT_EQ(expected_object, origin_objects[0]->value);
  EXPECT_FALSE(origin_objects[0]->incognito);

  std::vector<std::unique_ptr<ChooserContextBase::Object>> all_origin_objects =
      context->GetAllGrantedObjects();
  ASSERT_EQ(1u, all_origin_objects.size());
  EXPECT_EQ(foo_origin_.GetURL(), all_origin_objects[0]->requesting_origin);
  EXPECT_EQ(foo_origin_.GetURL(), all_origin_objects[0]->embedding_origin);
  EXPECT_EQ(expected_object, all_origin_objects[0]->value);
  EXPECT_FALSE(all_origin_objects[0]->incognito);

  testing::Mock::VerifyAndClearExpectations(&mock_permission_observer_);
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  EXPECT_CALL(mock_permission_observer_,
              OnPermissionRevoked(foo_origin_, foo_origin_));

  context->RevokeObjectPermission(foo_origin_, foo_origin_,
                                  origin_objects[0]->value);

  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, device_id));
  EXPECT_FALSE(context
                   ->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress())
                   .IsValid());

  origin_objects = context->GetGrantedObjects(foo_origin_, foo_origin_);
  EXPECT_EQ(0u, origin_objects.size());

  all_origin_objects = context->GetAllGrantedObjects();
  EXPECT_EQ(0u, all_origin_objects.size());
}

// Check that Web Bluetooth permissions granted in incognito mode remain only
// in the incognito session.
TEST_F(BluetoothChooserContextTest, GrantPermissionInIncognito) {
  const std::vector<BluetoothUUID> services{kGlucoseUUID, kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options =
      CreateOptionsForServices(services);

  BluetoothChooserContext* context = GetChooserContext(profile());
  BluetoothChooserContext* incognito_context =
      GetChooserContext(profile()->GetOffTheRecordProfile());

  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId device_id = context->GrantServiceAccessPermission(
      foo_origin_, foo_origin_, fake_device1_.get(), options);

  EXPECT_TRUE(
      context->HasDevicePermission(foo_origin_, foo_origin_, device_id));
  EXPECT_EQ(device_id,
            context->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress()));
  EXPECT_EQ(context->GetDeviceAddress(foo_origin_, foo_origin_, device_id),
            fake_device1_->GetAddress());
  EXPECT_TRUE(context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, device_id));
  for (const auto& service : services) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id, service));
  }

  EXPECT_FALSE(incognito_context->HasDevicePermission(foo_origin_, foo_origin_,
                                                      device_id));
  EXPECT_FALSE(incognito_context
                   ->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress())
                   .IsValid());

  testing::Mock::VerifyAndClearExpectations(&mock_permission_observer_);
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId incognito_device_id =
      incognito_context->GrantServiceAccessPermission(
          foo_origin_, foo_origin_, fake_device1_.get(), options);

  EXPECT_FALSE(context->HasDevicePermission(foo_origin_, foo_origin_,
                                            incognito_device_id));
  EXPECT_NE(incognito_device_id,
            context->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress()));
  EXPECT_TRUE(incognito_context->HasDevicePermission(foo_origin_, foo_origin_,
                                                     incognito_device_id));
  EXPECT_EQ(incognito_device_id,
            incognito_context->GetWebBluetoothDeviceId(
                foo_origin_, foo_origin_, fake_device1_->GetAddress()));
  EXPECT_EQ(incognito_context->GetDeviceAddress(foo_origin_, foo_origin_,
                                                incognito_device_id),
            fake_device1_->GetAddress());
  EXPECT_TRUE(incognito_context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, incognito_device_id));
  for (const auto& service : services) {
    EXPECT_TRUE(incognito_context->IsAllowedToAccessService(
        foo_origin_, foo_origin_, incognito_device_id, service));
  }

  {
    std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
        context->GetGrantedObjects(foo_origin_, foo_origin_);
    EXPECT_EQ(1u, origin_objects.size());

    std::vector<std::unique_ptr<ChooserContextBase::Object>>
        all_origin_objects = context->GetAllGrantedObjects();
    ASSERT_EQ(1u, all_origin_objects.size());
    EXPECT_FALSE(all_origin_objects[0]->incognito);
  }
  {
    std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
        incognito_context->GetGrantedObjects(foo_origin_, foo_origin_);
    EXPECT_EQ(1u, origin_objects.size());

    // GetAllGrantedObjects() on an incognito session also returns the
    // permission objects granted in the non-incognito session.
    std::vector<std::unique_ptr<ChooserContextBase::Object>>
        all_origin_objects = incognito_context->GetAllGrantedObjects();
    ASSERT_EQ(2u, all_origin_objects.size());
    EXPECT_TRUE(all_origin_objects[0]->incognito ^
                all_origin_objects[1]->incognito);
  }
}

// Check that granting device permission with new services updates the
// permission.
TEST_F(BluetoothChooserContextTest, CheckGrantWithServiceUpdates) {
  const std::vector<BluetoothUUID> services1{kGlucoseUUID, kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options1 =
      CreateOptionsForServices(services1);

  BluetoothChooserContext* context = GetChooserContext(profile());

  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId device_id1 =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options1);
  EXPECT_TRUE(context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, device_id1));
  for (const auto& service : services1) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id1, service));
  }

  const std::vector<BluetoothUUID> services2{kHeartRateUUID, kBloodPressureUUID,
                                             kCyclingPowerUUID};
  WebBluetoothRequestDeviceOptionsPtr options2 =
      CreateOptionsForServices(services2);

  testing::Mock::VerifyAndClearExpectations(&mock_permission_observer_);
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId device_id2 =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options2);
  EXPECT_EQ(device_id2, device_id1);

  base::flat_set<BluetoothUUID> services_set(services1);
  services_set.insert(services2.begin(), services2.end());
  for (const auto& service : services_set) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id2, service));
  }
}

// Check that permissions are granted to the union of filtered and optional
// services.
TEST_F(BluetoothChooserContextTest, CheckGrantWithOptionalServices) {
  const std::vector<BluetoothUUID> services{kGlucoseUUID, kBloodPressureUUID};
  const std::vector<BluetoothUUID> optional_services{kBatteryServiceUUID};
  WebBluetoothRequestDeviceOptionsPtr options =
      CreateOptionsForServices(services, optional_services);

  BluetoothChooserContext* context = GetChooserContext(profile());

  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId device_id = context->GrantServiceAccessPermission(
      foo_origin_, foo_origin_, fake_device1_.get(), options);

  EXPECT_TRUE(context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, device_id));
  for (const auto& service : services) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id, service));
  }
  for (const auto& service : optional_services) {
    EXPECT_TRUE(context->IsAllowedToAccessService(foo_origin_, foo_origin_,
                                                  device_id, service));
  }
}

// Check that the Bluetooth guard permission prevents Web Bluetooth from being
// used even if permissions exist for a pair of origins.
TEST_F(BluetoothChooserContextTest, BluetoothGuardPermission) {
  const std::vector<BluetoothUUID> services1{kGlucoseUUID, kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options1 =
      CreateOptionsForServices(services1);
  const std::vector<BluetoothUUID> services2{kHeartRateUUID, kCyclingPowerUUID};
  WebBluetoothRequestDeviceOptionsPtr options2 =
      CreateOptionsForServices(services2);

  auto* map = HostContentSettingsMapFactory::GetForProfile(profile());
  map->SetContentSettingDefaultScope(
      foo_url_, foo_url_, ContentSettingsType::BLUETOOTH_GUARD,
      /*resource_identifier=*/std::string(), CONTENT_SETTING_BLOCK);

  BluetoothChooserContext* context = GetChooserContext(profile());
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA))
      .Times(4);

  blink::WebBluetoothDeviceId foo_device_id1 =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options1);
  blink::WebBluetoothDeviceId foo_device_id2 =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options2);
  blink::WebBluetoothDeviceId bar_device_id1 =
      context->GrantServiceAccessPermission(bar_origin_, bar_origin_,
                                            fake_device1_.get(), options1);
  blink::WebBluetoothDeviceId bar_device_id2 =
      context->GrantServiceAccessPermission(bar_origin_, bar_origin_,
                                            fake_device2_.get(), options2);

  {
    std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
        context->GetGrantedObjects(foo_origin_, foo_origin_);
    EXPECT_EQ(0u, origin_objects.size());
  }
  {
    std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
        context->GetGrantedObjects(bar_origin_, bar_origin_);
    EXPECT_EQ(2u, origin_objects.size());
  }

  std::vector<std::unique_ptr<ChooserContextBase::Object>> all_origin_objects =
      context->GetAllGrantedObjects();
  EXPECT_EQ(2u, all_origin_objects.size());
  for (const auto& object : all_origin_objects) {
    EXPECT_EQ(object->requesting_origin, bar_origin_.GetURL());
    EXPECT_EQ(object->embedding_origin, bar_origin_.GetURL());
  }

  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, foo_device_id1));
  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, foo_device_id2));
  EXPECT_TRUE(
      context->HasDevicePermission(bar_origin_, bar_origin_, bar_device_id1));
  EXPECT_TRUE(
      context->HasDevicePermission(bar_origin_, bar_origin_, bar_device_id2));
}

// Check that a valid WebBluetoothDeviceId is produced for Bluetooth LE
// scanned devices. When permission is granted to one of these devices, the
// previously generated WebBluetoothDeviceId should be remembered.
TEST_F(BluetoothChooserContextTest, BluetoothLEScannedDevices) {
  BluetoothChooserContext* context = GetChooserContext(profile());

  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA))
      .Times(0);
  blink::WebBluetoothDeviceId scanned_id = context->AddScannedDevice(
      foo_origin_, foo_origin_, fake_device1_->GetAddress());

  EXPECT_EQ(scanned_id,
            context->GetWebBluetoothDeviceId(foo_origin_, foo_origin_,
                                             fake_device1_->GetAddress()));
  EXPECT_EQ(fake_device1_->GetAddress(),
            context->GetDeviceAddress(foo_origin_, foo_origin_, scanned_id));
  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, scanned_id));
  EXPECT_FALSE(context->IsAllowedToAccessAtLeastOneService(
      foo_origin_, foo_origin_, scanned_id));

  const std::vector<BluetoothUUID> services{kGlucoseUUID, kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options =
      CreateOptionsForServices(services);
  testing::Mock::VerifyAndClearExpectations(&mock_permission_observer_);
  EXPECT_CALL(mock_permission_observer_,
              OnChooserObjectPermissionChanged(
                  ContentSettingsType::BLUETOOTH_GUARD,
                  ContentSettingsType::BLUETOOTH_CHOOSER_DATA));
  blink::WebBluetoothDeviceId granted_id =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options);

  EXPECT_EQ(scanned_id, granted_id);
}

// Granted devices should return the same ID when detected via a Bluetooth LE
// scan. If the permission is revoked, then a new ID should be generated for the
// device when detected via a Bluetooth LE scan.
TEST_F(BluetoothChooserContextTest, BluetoothLEScanWithGrantedDevices) {
  const std::vector<BluetoothUUID> services{kGlucoseUUID, kBloodPressureUUID};
  WebBluetoothRequestDeviceOptionsPtr options =
      CreateOptionsForServices(services);

  BluetoothChooserContext* context = GetChooserContext(profile());

  blink::WebBluetoothDeviceId granted_id =
      context->GrantServiceAccessPermission(foo_origin_, foo_origin_,
                                            fake_device1_.get(), options);
  blink::WebBluetoothDeviceId scanned_id = context->AddScannedDevice(
      foo_origin_, foo_origin_, fake_device1_->GetAddress());
  EXPECT_EQ(granted_id, scanned_id);

  std::vector<std::unique_ptr<ChooserContextBase::Object>> origin_objects =
      context->GetGrantedObjects(foo_origin_, foo_origin_);
  ASSERT_EQ(1u, origin_objects.size());
  context->RevokeObjectPermission(foo_origin_, foo_origin_,
                                  origin_objects[0]->value);

  scanned_id = context->AddScannedDevice(foo_origin_, foo_origin_,
                                         fake_device1_->GetAddress());
  EXPECT_NE(scanned_id, granted_id);
  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, scanned_id));
  EXPECT_FALSE(
      context->HasDevicePermission(foo_origin_, foo_origin_, granted_id));
}
