#ifndef DOVI_LIBDOVI_CAPI_H
#define DOVI_LIBDOVI_CAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DoviRpuOpaque DoviRpuOpaque;

typedef struct DoviData {
	const uint8_t* data;
	size_t len;
} DoviData;

DoviRpuOpaque* dovi_parse_rpu(const uint8_t* data, size_t size);
DoviRpuOpaque* dovi_parse_unspec62_nalu(const uint8_t* data, size_t size);
const char* dovi_rpu_get_error(const DoviRpuOpaque* rpu);
int32_t dovi_rpu_get_profile(const DoviRpuOpaque* rpu);
int32_t dovi_rpu_get_el_type(const DoviRpuOpaque* rpu);
int32_t dovi_rpu_has_mapping(const DoviRpuOpaque* rpu);
int32_t dovi_rpu_has_cmv40_metadata(const DoviRpuOpaque* rpu);

int32_t dovi_convert_rpu_with_mode(DoviRpuOpaque* rpu, uint8_t mode);
int32_t dovi_convert_rpu_to_p81_preserve_mapping(DoviRpuOpaque* rpu);
int32_t dovi_rpu_remove_mapping(DoviRpuOpaque* rpu);
int32_t dovi_rpu_set_active_area_offsets(
	DoviRpuOpaque* rpu,
	uint16_t left,
	uint16_t right,
	uint16_t top,
	uint16_t bottom
);
int32_t dovi_rpu_add_cmv40_safe_default_metadata(DoviRpuOpaque* rpu);
int32_t dovi_rpu_remove_cmv40_metadata(DoviRpuOpaque* rpu);

const DoviData* dovi_write_unspec62_nalu(DoviRpuOpaque* rpu);
const DoviData* dovi_write_av1_rpu_metadata_obu_t35_payload(DoviRpuOpaque* rpu);
const DoviData* dovi_write_av1_rpu_metadata_obu_t35_complete(DoviRpuOpaque* rpu);
void dovi_data_free(const DoviData* data);
void dovi_rpu_free(DoviRpuOpaque* rpu);

#ifdef __cplusplus
}
#endif

#endif
