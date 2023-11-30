/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"
#include "app_config.h"
#include "fabric_table_delegate.h"
#include "led_util.h"

#include <platform/CHIPDeviceLayer.h>

#include "board_util.h"
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/examples/DeviceAttestationCredsExample.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/CodeUtils.h>
#include <system/SystemError.h>

#ifdef CONFIG_CHIP_WIFI
#include <app/clusters/network-commissioning/network-commissioning.h>
#include <platform/nrfconnect/wifi/NrfWiFiDriver.h>
#endif

#ifdef CONFIG_CHIP_OTA_REQUESTOR
#include "ota_util.h"
#endif

#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>


#include <platform/CHIPDeviceLayer.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <credentials/DeviceAttestationCredsProvider.h>


LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

namespace
{
	constexpr size_t kAppEventQueueSize = 10;
	constexpr uint32_t kFactoryResetTriggerTimeout = 6000;

	K_MSGQ_DEFINE(sAppEventQueue, sizeof(AppEvent), kAppEventQueueSize, alignof(AppEvent));
	k_timer sFunctionTimer;

	LEDWidget sStatusLED;
	FactoryResetLEDsWrapper<1> sFactoryResetLEDs{ { FACTORY_RESET_SIGNAL_LED } };
	bool sIsNetworkProvisioned = false;
	bool sIsNetworkEnabled = false;
	bool sHaveBLEConnections = false;

	const struct device *dev = DEVICE_DT_GET_ANY(bosch_bme680);
} /* namespace */

namespace LedConsts
{
	namespace StatusLed
	{
		namespace Unprovisioned
		{
			constexpr uint32_t kOn_ms{ 100 };
			constexpr uint32_t kOff_ms{ kOn_ms };
		} /* namespace Unprovisioned */
		namespace Provisioned
		{
			constexpr uint32_t kOn_ms{ 50 };
			constexpr uint32_t kOff_ms{ 950 };
		} /* namespace Provisioned */

	} /* namespace StatusLed */
} /* namespace LedConsts */


namespace Timers 
{
	k_timer sBMESensor;

	namespace FetchPeriodSeconds 
	{
		constexpr uint32_t sBMESensor{ 20 };
	} /* FetchPeriodSeconds */
} /* Timers */




#ifdef CONFIG_CHIP_WIFI
app::Clusters::NetworkCommissioning::Instance
	sWiFiCommissioningInstance(0, &(NetworkCommissioning::NrfWiFiDriver::Instance()));
#endif

CHIP_ERROR AppTask::Init()
{
	/* Initialize CHIP stack */
	LOG_INF("Init CHIP stack");

	CHIP_ERROR err = chip::Platform::MemoryInit();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("Platform::MemoryInit() failed");
		return err;
	}

	err = PlatformMgr().InitChipStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().InitChipStack() failed");
		return err;
	}

#if defined(CONFIG_NET_L2_OPENTHREAD)
	err = ThreadStackMgr().InitThreadStack();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ThreadStackMgr().InitThreadStack() failed: %s", ErrorStr(err));
		return err;
	}

#ifdef CONFIG_OPENTHREAD_MTD_SED
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_SleepyEndDevice);
#elif CONFIG_OPENTHREAD_MTD
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
#else
	err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_Router);
#endif /* CONFIG_OPENTHREAD_MTD_SED */
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("ConnectivityMgr().SetThreadDeviceType() failed: %s", ErrorStr(err));
		return err;
	}

#elif defined(CONFIG_CHIP_WIFI)
	sWiFiCommissioningInstance.Init();
#else
	return CHIP_ERROR_INTERNAL;
#endif /* CONFIG_NET_L2_OPENTHREAD */

	k_timer_init(&Timers::sBMESensor, &AppTask::BMEMeasurementTimeoutCallback, nullptr);
	k_timer_user_data_set(&Timers::sBMESensor, this);
	k_timer_start(&Timers::sBMESensor, K_NO_WAIT, K_SECONDS(Timers::FetchPeriodSeconds::sBMESensor));

	/* Initialize LEDs */
	LEDWidget::InitGpio();
	LEDWidget::SetStateUpdateCallback(LEDStateUpdateHandler);

	sStatusLED.Init(SYSTEM_STATE_LED);

	UpdateStatusLED();

	/* Initialize buttons */
	int ret = dk_buttons_init(ButtonEventHandler);
	if (ret) {
		LOG_ERR("dk_buttons_init() failed");
		return chip::System::MapErrorZephyr(ret);
	}

	/* Initialize function timer */
	k_timer_init(&sFunctionTimer, &AppTask::FunctionTimerTimeoutCallback, nullptr);
	k_timer_user_data_set(&sFunctionTimer, this);

#ifdef CONFIG_CHIP_OTA_REQUESTOR
	/* OTA image confirmation must be done before the factory data init. */
	OtaConfirmNewImage();
#endif

	/* Initialize CHIP server */
#if CONFIG_CHIP_FACTORY_DATA
	ReturnErrorOnFailure(mFactoryDataProvider.Init());
	SetDeviceInstanceInfoProvider(&mFactoryDataProvider);
	SetDeviceAttestationCredentialsProvider(&mFactoryDataProvider);
	SetCommissionableDataProvider(&mFactoryDataProvider);
#else
	SetDeviceInstanceInfoProvider(&DeviceInstanceInfoProviderMgrImpl());
	SetDeviceAttestationCredentialsProvider(Examples::GetExampleDACProvider());
#endif

	static chip::CommonCaseDeviceServerInitParams initParams;
	(void)initParams.InitializeStaticResourcesBeforeServerInit();

	ReturnErrorOnFailure(chip::Server::GetInstance().Init(initParams));
	ConfigurationMgr().LogDeviceConfig();
	PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
	AppFabricTableDelegate::Init();

	/*
	 * Add CHIP event handler and start CHIP thread.
	 * Note that all the initialization code should happen prior to this point to avoid data races
	 * between the main and the CHIP threads.
	 */
	PlatformMgr().AddEventHandler(ChipEventHandler, 0);

	err = PlatformMgr().StartEventLoopTask();
	if (err != CHIP_NO_ERROR) {
		LOG_ERR("PlatformMgr().StartEventLoopTask() failed");
		return err;
	}

	return CHIP_NO_ERROR;
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	AppEvent event = {};

	while (true) {
		k_msgq_get(&sAppEventQueue, &event, K_FOREVER);
		DispatchEvent(event);
	}

	return CHIP_NO_ERROR;
}
///////////////////////////////////////////////
void AppTask::BMEMeasurementTimeoutCallback(k_timer * timer) 
{
	if (!timer) return;

	AppEvent event;
	event.Type = AppEventType::SensorReadings;
	event.TimerEvent.Context = k_timer_user_data_get(timer);
	event.Handler = FunctionBMEFetchEventHandler;
	PostEvent(event);
}


void negative_check(int32_t *val, uint8_t *holder){ //You must give the holder a variable that has an initial value of 0, and if at the end the value has changed to 1 that means that you have to multiply the final number with -1.
    	if(*val < 0){
			*val =  *val * -1;   
			*holder = 1;
		}
	}

float true_value(int32_t val1, int32_t val2, uint8_t holder){
	float val = (float)val1 + (float)val2/1000000;
	if (holder==1){
		val = val * -1;
	}
	return val;
}

void AppTask::FunctionBMEFetchEventHandler(const AppEvent &event)
{
	struct sensor_value temp, hum, gas_res;
	uint8_t m1 = 0;
	uint8_t m2 = 0;
	uint8_t m3 = 0;
	float temp_value, hum_value, gas_value;


	sensor_sample_fetch(dev);
	sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
	sensor_channel_get(dev, SENSOR_CHAN_GAS_RES, &gas_res);

	negative_check(&temp.val1, &m1);
    negative_check(&temp.val1, &m1);
    temp_value = true_value(temp.val1, temp.val2, m1);
    
    negative_check(&hum.val1, &m2);
    negative_check(&hum.val2, &m2);
    hum_value = true_value(hum.val1, hum.val2, m2);

	negative_check(&gas_res.val1, &m3);
    negative_check(&gas_res.val2, &m3);
    gas_value = true_value(gas_res.val1, gas_res.val2, m3);
	
	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(
		static_cast<chip::EndpointId>(AppEventEndpointID::Temperature), static_cast<uint16_t>(temp.val1));

	chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(
		static_cast<chip::EndpointId>(AppEventEndpointID::Humidity), static_cast<uint16_t>(hum.val1));

	chip::app::Clusters::IlluminanceMeasurement::Attributes::MeasuredValue::Set(
		static_cast<chip::EndpointId>(AppEventEndpointID::AirQuality), static_cast<uint16_t>(gas_res.val1));

	// 	chip::app::Clusters::IlluminanceMeasurement::Attributes::MeasuredValue::Set(
	// 		static_cast<chip::EndpointId>(AppEventEndpointID::Illuminance), co2);
	// }
}
////////////////////////////////////////////////////


void AppTask::ButtonEventHandler(uint32_t buttonState, uint32_t hasChanged)
{
	AppEvent button_event;
	button_event.Type = AppEventType::Button;

	if (FUNCTION_BUTTON_MASK & hasChanged) {
		button_event.ButtonEvent.PinNo = FUNCTION_BUTTON;
		button_event.ButtonEvent.Action =
			static_cast<uint8_t>((FUNCTION_BUTTON_MASK & buttonState) ? AppEventType::ButtonPushed :
										    AppEventType::ButtonReleased);
		button_event.Handler = FunctionHandler;
		PostEvent(button_event);
	}
}

void AppTask::FunctionTimerTimeoutCallback(k_timer *timer)
{
	if (!timer) {
		return;
	}

	AppEvent event;
	event.Type = AppEventType::Timer;
	event.TimerEvent.Context = k_timer_user_data_get(timer);
	event.Handler = FunctionTimerEventHandler;
	PostEvent(event);
}

void AppTask::FunctionTimerEventHandler(const AppEvent &)
{
	if (Instance().mFunction == FunctionEvent::FactoryReset) {
		Instance().mFunction = FunctionEvent::NoneSelected;
		LOG_INF("Factory Reset triggered");

		sStatusLED.Set(true);
		sFactoryResetLEDs.Set(true);

		chip::Server::GetInstance().ScheduleFactoryReset();
	}
}

void AppTask::FunctionHandler(const AppEvent &event)
{
	if (event.ButtonEvent.PinNo != FUNCTION_BUTTON)
		return;

	if (event.ButtonEvent.Action == static_cast<uint8_t>(AppEventType::ButtonPushed)) {
		Instance().StartTimer(kFactoryResetTriggerTimeout);
		Instance().mFunction = FunctionEvent::FactoryReset;
	} else if (event.ButtonEvent.Action == static_cast<uint8_t>(AppEventType::ButtonReleased)) {
		if (Instance().mFunction == FunctionEvent::FactoryReset) {
			sFactoryResetLEDs.Set(false);
			UpdateStatusLED();
			Instance().CancelTimer();
			Instance().mFunction = FunctionEvent::NoneSelected;
			LOG_INF("Factory Reset has been Canceled");
		}
	}
}

void AppTask::LEDStateUpdateHandler(LEDWidget &ledWidget)
{
	AppEvent event;
	event.Type = AppEventType::UpdateLedState;
	event.Handler = UpdateLedStateEventHandler;
	event.UpdateLedStateEvent.LedWidget = &ledWidget;
	PostEvent(event);
}

void AppTask::UpdateLedStateEventHandler(const AppEvent &event)
{
	if (event.Type == AppEventType::UpdateLedState) {
		event.UpdateLedStateEvent.LedWidget->UpdateState();
	}
}

void AppTask::UpdateStatusLED()
{
	/* Update the status LED.
	 *
	 * If IPv6 networking and service provisioned, keep the LED On constantly.
	 *
	 * If the system has BLE connection(s) uptill the stage above, THEN blink the LED at an even
	 * rate of 100ms.
	 *
	 * Otherwise, blink the LED for a very short time. */
	if (sIsNetworkProvisioned && sIsNetworkEnabled) {
		sStatusLED.Set(true);
	} else if (sHaveBLEConnections) {
		sStatusLED.Blink(LedConsts::StatusLed::Unprovisioned::kOn_ms,
				 LedConsts::StatusLed::Unprovisioned::kOff_ms);
	} else {
		sStatusLED.Blink(LedConsts::StatusLed::Provisioned::kOn_ms, LedConsts::StatusLed::Provisioned::kOff_ms);
	}
}

void AppTask::ChipEventHandler(const ChipDeviceEvent *event, intptr_t /* arg */)
{
	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		sHaveBLEConnections = ConnectivityMgr().NumBLEConnections() != 0;
		UpdateStatusLED();
		break;
#if defined(CONFIG_NET_L2_OPENTHREAD)
	case DeviceEventType::kDnssdInitialized:
#if CONFIG_CHIP_OTA_REQUESTOR
		InitBasicOTARequestor();
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
		break;
	case DeviceEventType::kThreadStateChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsThreadProvisioned();
		sIsNetworkEnabled = ConnectivityMgr().IsThreadEnabled();
#elif defined(CONFIG_CHIP_WIFI)
	case DeviceEventType::kWiFiConnectivityChange:
		sIsNetworkProvisioned = ConnectivityMgr().IsWiFiStationProvisioned();
		sIsNetworkEnabled = ConnectivityMgr().IsWiFiStationEnabled();
#if CONFIG_CHIP_OTA_REQUESTOR
		if (event->WiFiConnectivityChange.Result == kConnectivity_Established) {
			InitBasicOTARequestor();
		}
#endif /* CONFIG_CHIP_OTA_REQUESTOR */
#endif
		UpdateStatusLED();
		break;
	default:
		break;
	}
}

void AppTask::CancelTimer()
{
	k_timer_stop(&sFunctionTimer);
}

void AppTask::StartTimer(uint32_t timeoutInMs)
{
	k_timer_start(&sFunctionTimer, K_MSEC(timeoutInMs), K_NO_WAIT);
}

void AppTask::PostEvent(const AppEvent &event)
{
	if (k_msgq_put(&sAppEventQueue, &event, K_NO_WAIT) != 0) {
		LOG_INF("Failed to post event to app task event queue");
	}
}

void AppTask::DispatchEvent(const AppEvent &event)
{
	if (event.Handler) {
		event.Handler(event);
	} else {
		LOG_INF("Event received with no handler. Dropping event.");
	}
}
