/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BCM2712 SoC die temperature (AVS monitor).
 *
 * AVS_RO_TEMP_STATUS at AVS +0x200: bits 16|10 = valid, bits 9:0 = raw
 * code, milli-Celsius = 450000 - 550 * raw. Same register and conversion
 * the platform's SsdtThermal.asl and ActiveCoolerDxe use. The register is
 * read-only status, so secure and non-secure reads never collide.
 *
 * Shared by the BMC I2C push service (bmc_sensor_pta) and the SCMI sensor
 * server (scmi_server); one translation unit owns the AVS mapping.
 */

#ifndef SOC_TEMP_H
#define SOC_TEMP_H

#include <stdbool.h>
#include <stdint.h>

/* Reads the die temperature in milli-Celsius. Returns false if invalid. */
bool soc_temp_read_mc(int32_t *milli_c);

#endif /* SOC_TEMP_H */
