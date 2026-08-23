// SPDX-License-Identifier: BSD-2-Clause
/*
 * bmc_sensord - normal-world consumer of the OP-TEE BMC sensor pTA.
 *
 * Two modes:
 *   bmc_sensord --once           print the latest cached sample and exit
 *   bmc_sensord [--period MS]    block on CMD_WAIT, printing each new sample
 *
 * The push to the BMC over I2C is autonomous inside OP-TEE; this daemon is
 * only needed to (a) re-run the RP1-BAR handshake if the OS moved the BAR,
 * and (b) observe/forward samples on the host side. It opens a session to
 * the pTA UUID and issues invoke commands exactly like the EDK2 handshake
 * driver does (see RpiOpteeSensorDxe).
 *
 * Build (needs optee_client's libteec + headers):
 *   cc -O2 -Wall bmc_sensord.c -lteec -o bmc_sensord
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tee_client_api.h>

/* Mirror of the pTA ABI (optee-os files/plat-rpi5/pta_bmc_sensor.h). */
#define PTA_BMC_SENSOR_UUID \
	{ 0x575d6607, 0x5a2b, 0x4384, \
	  { 0x82, 0x7e, 0xc1, 0x6a, 0x25, 0xac, 0x4f, 0xa5 } }

#define PTA_BMC_SENSOR_CMD_INIT	0
#define PTA_BMC_SENSOR_CMD_GET	1
#define PTA_BMC_SENSOR_CMD_WAIT	2

/*
 * The pi-bmc wire contract. Only needed by --init: on the RPi 5, EDK2
 * already ran the handshake, so a plain --once / wait never sends these.
 */
#define RP1_BAR_DEFAULT		0x1f00000000ULL
#define RP1_I2C1_OFFSET		0x74000
#define BMC_EEPROM_SLAVE	0x50
#define BMC_SENSOR_EEPROM_OFF	0x7800

static const TEEC_UUID pta_uuid = PTA_BMC_SENSOR_UUID;

static void print_sample(uint32_t temp_mc, uint32_t seq, uint32_t status,
			 uint32_t errs)
{
	printf("seq=%u soc=%d.%03d C status=0x%x i2c_errs=%u\n",
	       seq, (int32_t)temp_mc / 1000, abs((int32_t)temp_mc % 1000),
	       status, errs);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	TEEC_Context ctx;
	TEEC_Session sess;
	TEEC_Operation op;
	TEEC_Result res;
	uint32_t origin = 0;
	int once = 0, do_init = 0;
	uint32_t period = 0;
	unsigned long long bar = RP1_BAR_DEFAULT;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--init"))
			do_init = 1;
		else if (!strcmp(argv[i], "--period") && i + 1 < argc)
			period = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--bar") && i + 1 < argc)
			bar = strtoull(argv[++i], NULL, 0);
		else {
			fprintf(stderr,
				"usage: %s [--once] [--init [--bar ADDR]] "
				"[--period MS]\n", argv[0]);
			return 2;
		}
	}

	res = TEEC_InitializeContext(NULL, &ctx);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_InitializeContext: 0x%x\n", res);
		return 1;
	}

	res = TEEC_OpenSession(&ctx, &sess, &pta_uuid, TEEC_LOGIN_PUBLIC,
			       NULL, NULL, &origin);
	if (res != TEEC_SUCCESS) {
		fprintf(stderr, "TEEC_OpenSession: 0x%x (origin 0x%x)\n",
			res, origin);
		TEEC_FinalizeContext(&ctx);
		return 1;
	}

	if (do_init) {
		memset(&op, 0, sizeof(op));
		op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
						 TEEC_VALUE_INPUT,
						 TEEC_VALUE_INPUT,
						 TEEC_NONE);
		op.params[0].value.a = (uint32_t)bar;
		op.params[0].value.b = (uint32_t)(bar >> 32);
		op.params[1].value.a = RP1_I2C1_OFFSET;
		op.params[1].value.b = BMC_EEPROM_SLAVE;
		op.params[2].value.a = BMC_SENSOR_EEPROM_OFF;
		op.params[2].value.b = period;
		res = TEEC_InvokeCommand(&sess, PTA_BMC_SENSOR_CMD_INIT, &op,
					 &origin);
		if (res != TEEC_SUCCESS) {
			fprintf(stderr, "CMD_INIT: 0x%x (origin 0x%x)\n",
				res, origin);
			goto out;
		}
	}

	if (once) {
		memset(&op, 0, sizeof(op));
		op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_OUTPUT,
						 TEEC_VALUE_OUTPUT,
						 TEEC_VALUE_OUTPUT,
						 TEEC_NONE);
		res = TEEC_InvokeCommand(&sess, PTA_BMC_SENSOR_CMD_GET, &op,
					 &origin);
		if (res == TEEC_SUCCESS)
			print_sample(op.params[0].value.a, op.params[0].value.b,
				     op.params[1].value.a, op.params[1].value.b);
		else
			fprintf(stderr, "CMD_GET: 0x%x\n", res);
		goto out;
	}

	/* Streaming: block until each new sample arrives. */
	{
		uint32_t last_seq = 0;

		for (;;) {
			memset(&op, 0, sizeof(op));
			op.paramTypes = TEEC_PARAM_TYPES(TEEC_VALUE_INPUT,
							 TEEC_VALUE_OUTPUT,
							 TEEC_VALUE_OUTPUT,
							 TEEC_NONE);
			op.params[0].value.a = last_seq;
			op.params[0].value.b = 0; /* wait forever */
			res = TEEC_InvokeCommand(&sess, PTA_BMC_SENSOR_CMD_WAIT,
						 &op, &origin);
			if (res != TEEC_SUCCESS) {
				fprintf(stderr, "CMD_WAIT: 0x%x\n", res);
				break;
			}
			last_seq = op.params[1].value.b;
			print_sample(op.params[1].value.a, op.params[1].value.b,
				     op.params[2].value.a, op.params[2].value.b);
		}
	}

out:
	TEEC_CloseSession(&sess);
	TEEC_FinalizeContext(&ctx);
	return res == TEEC_SUCCESS ? 0 : 1;
}
