#include <stdint.h>

#include "zlink.h"

#ifdef __cplusplus
#define ZLINK_JAVA_EXPORT extern "C"
#else
#define ZLINK_JAVA_EXPORT
#endif
ZLINK_JAVA_EXPORT uintptr_t zlink_java_msg_data_addr (zlink_msg_t *msg)
{
    return (uintptr_t) zlink_msg_data (msg);
}
