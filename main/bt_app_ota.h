#pragma once

#ifdef CONFIG_EXAMPLE_OTA_ENABLE
void try_ota_update(const char *hostname, const char *app_name, const char *ota_url);
#endif