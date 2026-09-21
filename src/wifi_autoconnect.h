#ifndef WIFI_AUTOCONNECT_H_
#define WIFI_AUTOCONNECT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int wifi_autoconnect_start(void);
void wifi_autoconnect_poll(void);
bool wifi_autoconnect_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_AUTOCONNECT_H_ */
