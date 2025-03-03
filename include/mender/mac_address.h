#ifndef __MAC_ADDRESS_PRIV_H__
#define __MAC_ADDRESS_PRIV_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "utils.h"

mender_err_t mender_storage_set_mac_address(const char *mac_address);

mender_err_t mender_storage_get_mac_address(char **mac_address);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __MAC_ADDRESS_PRIV_H__ */

