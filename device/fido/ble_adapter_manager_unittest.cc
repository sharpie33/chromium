// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/fido/ble_adapter_manager.h"

#include <map>
#include <memory>
#include <utility>

#include "base/bind_helpers.h"
#include "base/run_loop.h"
#include "base/test/gmock_callback_support.h"
#include "base/test/task_environment.h"
#include "device/bluetooth/bluetooth_adapter_factory.h"
#include "device/bluetooth/test/mock_bluetooth_adapter.h"
#include "device/bluetooth/test/mock_bluetooth_device.h"
#include "device/fido/fake_fido_discovery.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_request_handler_base.h"
#include "device/fido/fido_transport_protocol.h"
#include "device/fido/test_callback_receiver.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace device {

namespace {

using base::test::RunOnceClosure;
using ::testing::_;

constexpr char kTestBluetoothDeviceAddress[] = "test_device_address";
constexpr char kTestFidoBleAuthenticatorId[] = "ble:test_device_address";
constexpr char kTestPinCode[] = "1234";
constexpr char kTestBluetoothDisplayName[] = "device_name";

class MockObserver : public FidoRequestHandlerBase::Observer {
 public:
  MockObserver() = default;
  ~MockObserver() override = default;

  MOCK_METHOD1(OnTransportAvailabilityEnumerated,
               void(FidoRequestHandlerBase::TransportAvailabilityInfo data));
  MOCK_METHOD1(EmbedderControlsAuthenticatorDispatch,
               bool(const FidoAuthenticator& authenticator));
  MOCK_METHOD1(BluetoothAdapterPowerChanged, void(bool is_powered_on));
  MOCK_METHOD1(FidoAuthenticatorAdded,
               void(const FidoAuthenticator& authenticator));
  MOCK_METHOD1(FidoAuthenticatorRemoved, void(base::StringPiece device_id));
  MOCK_METHOD2(FidoAuthenticatorIdChanged,
               void(base::StringPiece old_authenticator_id,
                    std::string new_authenticator_id));
  MOCK_METHOD3(FidoAuthenticatorPairingModeChanged,
               void(base::StringPiece, bool, base::string16));
  MOCK_CONST_METHOD0(SupportsPIN, bool());
  MOCK_METHOD2(CollectPIN,
               void(base::Optional<int>,
                    base::OnceCallback<void(std::string)>));
  MOCK_METHOD0(FinishCollectToken, void());
  MOCK_METHOD1(SetMightCreateResidentCredential, void(bool));

 private:
  DISALLOW_COPY_AND_ASSIGN(MockObserver);
};

class FakeFidoRequestHandlerBase : public FidoRequestHandlerBase {
 public:
  FakeFidoRequestHandlerBase(MockObserver* observer,
                             FidoDiscoveryFactory* fido_discovery_factory)
      : FidoRequestHandlerBase(fido_discovery_factory,
                               {FidoTransportProtocol::kBluetoothLowEnergy}) {
    set_observer(observer);
    Start();
  }

  void SimulateFidoRequestHandlerHasAuthenticator(bool simulate_authenticator) {
    simulate_authenticator_ = simulate_authenticator;
  }

 private:
  void DispatchRequest(FidoAuthenticator*) override {}

  bool HasAuthenticator(
      const std::string& authentictator_address) const override {
    return simulate_authenticator_;
  }

  bool simulate_authenticator_ = false;

  DISALLOW_COPY_AND_ASSIGN(FakeFidoRequestHandlerBase);
};

}  // namespace

class FidoBleAdapterManagerTest : public ::testing::Test {
 public:
  FidoBleAdapterManagerTest() {
    BluetoothAdapterFactory::SetAdapterForTesting(adapter_);
    fido_discovery_factory_->ForgeNextBleDiscovery(
        test::FakeFidoDiscovery::StartMode::kAutomatic);

    fake_request_handler_ = std::make_unique<FakeFidoRequestHandlerBase>(
        mock_observer_.get(), fido_discovery_factory_.get());
  }

  MockBluetoothDevice* AddMockBluetoothDeviceToAdapter() {
    auto mock_bluetooth_device = std::make_unique<MockBluetoothDevice>(
        adapter_.get(), 0 /* bluetooth_class */, kTestBluetoothDisplayName,
        kTestBluetoothDeviceAddress, false /* paired */, false /* connected */);

    auto* mock_bluetooth_device_ptr = mock_bluetooth_device.get();
    adapter_->AddMockDevice(std::move(mock_bluetooth_device));
    return mock_bluetooth_device_ptr;
  }

  MockBluetoothAdapter* adapter() { return adapter_.get(); }
  MockObserver* observer() { return mock_observer_.get(); }
  bool adapter_powered_on_programmatically(
      const BleAdapterManager& adapter_manager) {
    return adapter_manager.adapter_powered_on_programmatically_;
  }

  FakeFidoRequestHandlerBase* fake_request_handler() {
    return fake_request_handler_.get();
  }

  const base::flat_map<std::string, std::string>& device_pincode_map(
      const FidoBlePairingDelegate& delegate) const {
    return delegate.bluetooth_device_pincode_map_;
  }

  const FidoBlePairingDelegate& ble_pairing_delegate(
      const BleAdapterManager& ble_adapter_manager) {
    return ble_adapter_manager.pairing_delegate_;
  }

 protected:
  base::test::TaskEnvironment task_environment_;
  scoped_refptr<MockBluetoothAdapter> adapter_ =
      base::MakeRefCounted<::testing::NiceMock<MockBluetoothAdapter>>();
  std::unique_ptr<MockObserver> mock_observer_ =
      std::make_unique<MockObserver>();
  std::unique_ptr<test::FakeFidoDiscoveryFactory> fido_discovery_factory_ =
      std::make_unique<test::FakeFidoDiscoveryFactory>();

  std::unique_ptr<FakeFidoRequestHandlerBase> fake_request_handler_;
};

TEST_F(FidoBleAdapterManagerTest, AdapterNotPresent) {
  EXPECT_CALL(*adapter(), IsPresent()).WillOnce(::testing::Return(false));
  EXPECT_CALL(*adapter(), IsPowered()).WillOnce(::testing::Return(false));
  EXPECT_CALL(*adapter(), CanPower()).WillOnce(::testing::Return(false));

  FidoRequestHandlerBase::TransportAvailabilityInfo data;
  EXPECT_CALL(*observer(), OnTransportAvailabilityEnumerated(_))
      .WillOnce(::testing::SaveArg<0>(&data));

  task_environment_.RunUntilIdle();

  EXPECT_FALSE(data.is_ble_powered);
  EXPECT_FALSE(data.can_power_on_ble_adapter);
}

TEST_F(FidoBleAdapterManagerTest, AdapaterPresentAndPowered) {
  EXPECT_CALL(*adapter(), IsPresent()).WillOnce(::testing::Return(true));
  EXPECT_CALL(*adapter(), IsPowered()).WillOnce(::testing::Return(true));
  EXPECT_CALL(*adapter(), CanPower()).WillOnce(::testing::Return(false));

  FidoRequestHandlerBase::TransportAvailabilityInfo data;
  EXPECT_CALL(*observer(), OnTransportAvailabilityEnumerated(_))
      .WillOnce(::testing::SaveArg<0>(&data));

  task_environment_.RunUntilIdle();

  EXPECT_TRUE(data.is_ble_powered);
  EXPECT_FALSE(data.can_power_on_ble_adapter);
}

TEST_F(FidoBleAdapterManagerTest, AdapaterPresentAndCanBePowered) {
  EXPECT_CALL(*adapter(), IsPresent).WillOnce(::testing::Return(true));
  EXPECT_CALL(*adapter(), IsPowered).WillOnce(::testing::Return(false));
  EXPECT_CALL(*adapter(), CanPower).WillOnce(::testing::Return(true));

  FidoRequestHandlerBase::TransportAvailabilityInfo data;
  EXPECT_CALL(*observer(), OnTransportAvailabilityEnumerated(_))
      .WillOnce(::testing::SaveArg<0>(&data));

  task_environment_.RunUntilIdle();

  EXPECT_FALSE(data.is_ble_powered);
  EXPECT_TRUE(data.can_power_on_ble_adapter);
}

TEST_F(FidoBleAdapterManagerTest, SetBluetoothPowerOn) {
  task_environment_.RunUntilIdle();
  auto& power_manager =
      fake_request_handler_->get_bluetooth_adapter_manager_for_testing();
  ::testing::InSequence s;
  EXPECT_CALL(*adapter(), SetPowered(true, _, _));
  EXPECT_CALL(*adapter(), SetPowered(false, _, _));
  power_manager->SetAdapterPower(true /* set_power_on */);
  EXPECT_TRUE(adapter_powered_on_programmatically(*power_manager));
  power_manager.reset();
}

TEST_F(FidoBleAdapterManagerTest, SuccessfulPairing) {
  fake_request_handler()->SimulateFidoRequestHandlerHasAuthenticator(
      true /* simulate_authenticator */);
  auto* mock_bluetooth_device = AddMockBluetoothDeviceToAdapter();

  EXPECT_CALL(*adapter(), GetDevices())
      .WillRepeatedly(::testing::Return(adapter()->GetConstMockDevices()));
  EXPECT_CALL(*mock_bluetooth_device, Pair_)
      .WillOnce(::testing::WithArgs<0, 1>(
          [mock_bluetooth_device](BluetoothDevice::PairingDelegate* delegate,
                                  base::OnceClosure& success_callback) {
            delegate->RequestPinCode(mock_bluetooth_device);
            std::move(success_callback).Run();
          }));
  EXPECT_CALL(*mock_bluetooth_device, SetPinCode(kTestPinCode));

  task_environment_.RunUntilIdle();
  auto& adapter_manager =
      fake_request_handler_->get_bluetooth_adapter_manager_for_testing();
  test::TestCallbackReceiver<> callback_receiver;
  adapter_manager->InitiatePairing(kTestFidoBleAuthenticatorId, kTestPinCode,
                                   callback_receiver.callback(),
                                   base::DoNothing());
  callback_receiver.WaitForCallback();

  const auto& pin_code_map =
      device_pincode_map(ble_pairing_delegate(*adapter_manager));
  EXPECT_EQ(1u, pin_code_map.size());
  ASSERT_TRUE(base::Contains(pin_code_map, kTestFidoBleAuthenticatorId));
  EXPECT_EQ(kTestPinCode,
            pin_code_map.find(kTestFidoBleAuthenticatorId)->second);
}

TEST_F(FidoBleAdapterManagerTest, PairingFailsOnUnknownDevice) {
  auto* mock_bluetooth_device = AddMockBluetoothDeviceToAdapter();

  EXPECT_CALL(*adapter(), GetDevices())
      .WillRepeatedly(::testing::Return(adapter()->GetConstMockDevices()));
  EXPECT_CALL(*mock_bluetooth_device, Pair_).Times(0);

  task_environment_.RunUntilIdle();
  auto& power_manager =
      fake_request_handler_->get_bluetooth_adapter_manager_for_testing();
  test::TestCallbackReceiver<> callback_receiver;
  power_manager->InitiatePairing(kTestFidoBleAuthenticatorId, kTestPinCode,
                                 base::DoNothing(),
                                 callback_receiver.callback());
  callback_receiver.WaitForCallback();

  const auto& pin_code_map =
      device_pincode_map(ble_pairing_delegate(*power_manager));
  EXPECT_TRUE(pin_code_map.empty());
}

TEST_F(FidoBleAdapterManagerTest, PairingCancelledOnDestruction) {
  fake_request_handler()->SimulateFidoRequestHandlerHasAuthenticator(
      true /* simulate_authenticator */);
  auto* mock_bluetooth_device = AddMockBluetoothDeviceToAdapter();

  EXPECT_CALL(*adapter(), GetDevices())
      .WillRepeatedly(::testing::Return(adapter()->GetConstMockDevices()));
  EXPECT_CALL(*mock_bluetooth_device, Pair_).WillOnce(RunOnceClosure<1>());

  task_environment_.RunUntilIdle();
  auto& adapter_manager =
      fake_request_handler_->get_bluetooth_adapter_manager_for_testing();
  test::TestCallbackReceiver<> callback_receiver;
  adapter_manager->InitiatePairing(kTestFidoBleAuthenticatorId, kTestPinCode,
                                   callback_receiver.callback(),
                                   base::DoNothing());
  callback_receiver.WaitForCallback();

  const auto& pin_code_map =
      device_pincode_map(ble_pairing_delegate(*adapter_manager));
  EXPECT_EQ(1u, pin_code_map.size());
  ASSERT_TRUE(base::Contains(pin_code_map, kTestFidoBleAuthenticatorId));
  EXPECT_EQ(kTestPinCode,
            pin_code_map.find(kTestFidoBleAuthenticatorId)->second);

  // Destroying BleAdapterManager should call CancelPairing() on all
  // BluetoothDevice which has been attempted to be paried by the pairing
  // delegate.
  testing::Mock::VerifyAndClearExpectations(mock_bluetooth_device);
  EXPECT_CALL(*mock_bluetooth_device, CancelPairing);
  adapter_manager.reset();
}

}  // namespace device
