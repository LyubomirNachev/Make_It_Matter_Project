// /*
//  * Copyright (c) 2021 Nordic Semiconductor ASA
//  *
//  * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
//  */

// #include <lib/support/logging/CHIPLogging.h>

// #include "app_task.h"
// #include "app_config.h"

// #include <app-common/zap-generated/attributes/Accessors.h>
// #include <app-common/zap-generated/ids/Attributes.h>
// #include <app-common/zap-generated/ids/Clusters.h>
// #include <app/ConcreteAttributePath.h>


// #include <zephyr/kernel.h>
// #include <zephyr/drivers/gpio.h>


// #define SLEEP_TIME_MS   1000

// using namespace ::chip;
// using namespace ::chip::app::Clusters;
// using namespace ::chip::app::Clusters::OnOff;

// void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath, uint8_t type,
// 				     				   uint16_t size, uint8_t *value)
// {
// if (attributePath.mClusterId == OnOff::Id && attributePath.mAttributeId == OnOff::Attributes::OnOff::Id) {
//         gpio_pin_set_dt(&led, *value);
//     }
// }

// /** @brief OnOff Cluster Init
//  *
//  * This function is called when a specific cluster is initialized. It gives the
//  * application an opportunity to take care of cluster initialization procedures.
//  * It is called exactly once for each endpoint where cluster is present.
//  *
//  * @param endpoint   Ver.: always
//  *
//  * TODO Issue #3841
//  * emberAfOnOffClusterInitCallback happens before the stack initialize the cluster
//  * attributes to the default value.
//  * The logic here expects something similar to the deprecated Plugins callback
//  * emberAfPluginOnOffClusterServerPostInitCallback.
//  *
//  */
// void emberAfOnOffClusterInitCallback(EndpointId endpoint)
// {
// 	EmberAfStatus status;
// 	bool storedValue;

// 	/* Read storedValue on/off value */
// 	status = Attributes::OnOff::Get(endpoint, &storedValue);
// 	if (status == EMBER_ZCL_STATUS_SUCCESS) {
//         int ret;

//         if (!device_is_ready(led.port)) {
//             return;
//         }

//         ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
//         if (ret < 0) {
//             return;
//         }
// 	}
// }
