/*
 * CDK custom config for EdgeNOS-4610 datapath app.
 *
 * Based on OpenMDK examples/linux-user/cdk_custom_config.h (system libc),
 * but scoped to the AS4610-54T's chip (Broadcom BCM56340 "Helix4") so the
 * build is small and focused instead of pulling in every supported device.
 *
 * Governed by the Broadcom Switch APIs license (see ../../../LICENSING.md).
 */
#ifndef __CDK_CUSTOM_CONFIG_H__
#define __CDK_CUSTOM_CONFIG_H__

/* ---- Scope the build to our chip only (pattern from examples/cdksim) ---- */
#define CDK_CONFIG_INCLUDE_CHIP_DEFAULT         0
#define CDK_CONFIG_INCLUDE_BCM56340             1
/* keep symbols + field info so the shell can decode regs/mems for debugging */
#define CDK_CONFIG_INCLUDE_CHIP_SYMBOLS         1
#define CDK_CONFIG_INCLUDE_FIELD_INFO           1

/* ---- Use the system built-in data types ---- */
#include <inttypes.h>
#include <stdlib.h>

#define CDK_CONFIG_DEFINE_SIZE_T                0
#define CDK_CONFIG_DEFINE_UINT8_T               0
#define CDK_CONFIG_DEFINE_UINT16_T              0
#define CDK_CONFIG_DEFINE_UINT32_T              0
#define CDK_CONFIG_DEFINE_PRIu32                0
#define CDK_CONFIG_DEFINE_PRIx32                0

/* ---- Use the system-provided LIBC ---- */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define CDK_ABORT                               abort
#define CDK_PRINTF                              printf
#define CDK_VPRINTF                             vprintf
#define CDK_SPRINTF                             sprintf
#define CDK_VSPRINTF                            vsprintf
#define CDK_ATOI                                atoi
#define CDK_STRNCHR                             strnchr
#define CDK_STRCPY                              strcpy
#define CDK_STRNCPY                             strncpy
#define CDK_STRLEN                              strlen
#define CDK_STRCHR                              strchr
#define CDK_STRRCHR                             strrchr
#define CDK_STRCMP                              strcmp
#define CDK_MEMCMP                              memcmp
#define CDK_MEMSET                              memset
#define CDK_MEMCPY                              memcpy
#define CDK_STRUPR                              strupr
#define CDK_TOUPPER                             toupper
#define CDK_STRCAT                              strcat

#endif /* __CDK_CUSTOM_CONFIG_H__ */
