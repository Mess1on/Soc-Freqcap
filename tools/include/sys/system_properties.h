/* 非 NDK 构建用的最小声明：bionic 的 <sys/system_properties.h> 只有
 * __system_property_get 被本仓库用到；实现见 tools/props_fallback.c。 */
#ifndef SF_TOOLS_SYSTEM_PROPERTIES_H
#define SF_TOOLS_SYSTEM_PROPERTIES_H

#ifdef __cplusplus
extern "C" {
#endif

int __system_property_get(const char *name, char *value);

#ifdef __cplusplus
}
#endif

#endif
