#pragma once

#include "sdkconfig.h"
#ifndef CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE

#include <stdint.h>

/*
* Initializes volume control data structures.
* Allowed dB values in the range of -96 to 0.
*/
void bt_app_vc_initialize(float min_db, float max_db, bool level0_mute);

/*
* Sets volume. Allowed range is 0-127
*/
void bt_app_set_volume(uint32_t level);

/*
* Gets volume (range 0-127)
*/
uint32_t bt_app_get_volume(void);

/*
* Sets initial volume.
*/
void bt_app_set_initial_volume();

/*
* Changes an input data according to volume level. 
*/
void bt_app_adjust_volume(uint8_t *data, size_t size);

#endif /* CONFIG_EXAMPLE_BUILD_FACTORY_IMAGE */