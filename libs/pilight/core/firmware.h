/*
	Copyright (C) 2014 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#ifndef _FIRMWARE_H_
#define _FIRMWARE_H_

typedef struct firmware_t {
	double version;		// firmware version (cooked number).
	double lpf;		// minimum pulse lenght [us] that passes the filter.
	double hpf;		// maximum pulse length [us] that passes the filter.
	const char *method;	// filter method (since version 4)
	double raw_version;	// version number as received from firmware.
} firmware_t;
firmware_t firmware;

typedef enum {
	FW_PROG_OP_FAIL = 1,
	FW_INIT_FAIL,
	FW_RD_SIG_FAIL,
	FW_INV_SIG_FAIL,
	FW_MATCH_SIG_FAIL,
	FW_ERASE_FAIL,
	FW_WRITE_FAIL,
	FW_VERIFY_FAIL,
	FW_RD_FUSE_FAIL,
	FW_INV_FUSE_FAIL
} exitrc_t;

typedef enum {
	FW_MP_UNKNOWN,
	FW_MP_ATTINY25,
	FW_MP_ATTINY45,
	FW_MP_ATTINY85,
	FW_MP_ATMEL328P,
	FW_MP_ATMEL32U4
} mptype_t;

firmware_t *firmware_from_hw(firmware_t *result, double version, double lpf, double hpf);
firmware_t *firmware_from_json(firmware_t *fw, const JsonNode *source);
void firmware_free_json(firmware_t *fw);	// must be called after firmware_from_json().
JsonNode *firmware_to_json(const firmware_t *fw, JsonNode *target);
void firmware_to_registry(const firmware_t *fw);

int firmware_update(char *fwfile, char *comport);
int firmware_getmp(char *comport);

#endif
