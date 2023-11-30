
#include <lib/support/logging/CHIPLogging.h>

#include "app_task.h"
#include "app_config.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>


#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>


#define SLEEP_TIME_MS   1000

using namespace ::chip;
using namespace ::chip::app::Clusters;
using namespace ::chip::app::Clusters::OnOff;

void MatterPostAttributeChangeCallback(const chip::app::ConcreteAttributePath &attributePath, uint8_t type,
				     				   uint16_t size, uint8_t *value)
{
    if (attributePath.mClusterId == OnOff::Id && attributePath.mAttributeId == OnOff::Attributes::OnOff::Id && attributePath.mEndpointId == 1) {
            gpio_pin_set_dt(&relay, *value);
        }
    else if(attributePath.mClusterId == OnOff::Id && attributePath.mAttributeId == OnOff::Attributes::OnOff::Id && attributePath.mEndpointId == 2){
            gpio_pin_set_dt(&relay_second, *value);
        }
}

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
	EmberAfStatus status;
	bool storedValue;

	/* Read storedValue on/off value */
	status = Attributes::OnOff::Get(endpoint, &storedValue);
	if (status == EMBER_ZCL_STATUS_SUCCESS) {
        int ret;

        if (!device_is_ready(relay.port)) {
            return;
        }

        ret = gpio_pin_configure_dt(&relay, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
        if (ret < 0) {
            return;
        }
        int ret1;

        if (!device_is_ready(relay_second.port)) {
            return;
        }

        ret1 = gpio_pin_configure_dt(&relay_second, GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
        if (ret1 < 0) {
            return;
        }
    }
}