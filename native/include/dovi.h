#ifndef DOVI_H
#define DOVI_H

#include <stdint.h>

#if defined(_WIN32)
#define DOVI_EXPORT __declspec(dllexport)
#else
#define DOVI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DOVI_ABI_VERSION 3u

typedef int32_t dovi_status;

#define DOVI_OK ((dovi_status)0)
#define DOVI_INVALID_ARGUMENT ((dovi_status)-1)
#define DOVI_MALFORMED_SAMPLE ((dovi_status)-2)
#define DOVI_OUTPUT_TOO_SMALL ((dovi_status)-3)
#define DOVI_RPU_PARSE_FAILED ((dovi_status)-4)
#define DOVI_RPU_CONVERT_FAILED ((dovi_status)-5)
#define DOVI_RPU_WRITE_FAILED ((dovi_status)-6)
#define DOVI_UNSUPPORTED_FRAMING ((dovi_status)-7)
#define DOVI_INTERNAL_ERROR ((dovi_status)-8)
#define DOVI_RPU_NOT_FOUND ((dovi_status)-9)
#define DOVI_UNSUPPORTED_PROFILE ((dovi_status)-10)
#define DOVI_REENCODE_REQUIRED ((dovi_status)-11)
#define DOVI_REPAIR_FAILED ((dovi_status)-12)
#define DOVI_INCONSISTENT_RPU ((dovi_status)-13)

typedef enum dovi_framing {
	DOVI_FRAMING_AUTO = 0,
	DOVI_FRAMING_ANNEX_B = 1,
	DOVI_FRAMING_LENGTH_PREFIXED = 2,
} dovi_framing;

typedef enum dovi_rpu_format {
	DOVI_RPU_FORMAT_RAW = 0,
	DOVI_RPU_FORMAT_UNSPEC62_NAL = 1,
} dovi_rpu_format;

typedef enum dovi_target {
	DOVI_TARGET_LOSSLESS_REWRITE = 0,
	DOVI_TARGET_MEL = 1,
	DOVI_TARGET_PROFILE_8_1 = 2,
	DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING = 3,
	DOVI_TARGET_PROFILE_8_4 = 4,
	DOVI_TARGET_SOURCE_BASE_PRESENTATION = 5,
} dovi_target;

typedef enum dovi_repair_flags {
	DOVI_REPAIR_NONE = 0,
	DOVI_REPAIR_REMOVE_MAPPING = 1u << 0,
	DOVI_REPAIR_ZERO_ACTIVE_AREA = 1u << 1,
	DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS = 1u << 2,
	DOVI_REPAIR_REMOVE_CMV40 = 1u << 3,
} dovi_repair_flags;

typedef enum dovi_presentation {
	DOVI_PRESENTATION_UNKNOWN = 0,
	DOVI_PRESENTATION_PROFILE_5 = 1,
	DOVI_PRESENTATION_PROFILE_7_MEL = 2,
	DOVI_PRESENTATION_PROFILE_7_FEL = 3,
	DOVI_PRESENTATION_PROFILE_8_1 = 4,
	DOVI_PRESENTATION_PROFILE_8_4 = 5,
	DOVI_PRESENTATION_HDR10 = 6,
	DOVI_PRESENTATION_HDR10_PLUS = 7,
	DOVI_PRESENTATION_HLG = 8,
} dovi_presentation;

typedef enum dovi_capability_flags {
	DOVI_CAP_INSPECT = 1ull << 0,
	DOVI_CAP_VALIDATE = 1ull << 1,
	DOVI_CAP_LOSSLESS_REWRITE = 1ull << 2,
	DOVI_CAP_MEL = 1ull << 3,
	DOVI_CAP_PROFILE_8_1 = 1ull << 4,
	DOVI_CAP_PROFILE_8_1_PRESERVE_MAPPING = 1ull << 5,
	DOVI_CAP_PROFILE_8_4 = 1ull << 6,
	DOVI_CAP_SOURCE_BASE_PRESENTATION = 1ull << 7,
	DOVI_CAP_REPAIR_REMOVE_MAPPING = 1ull << 8,
	DOVI_CAP_REPAIR_ZERO_ACTIVE_AREA = 1ull << 9,
	DOVI_CAP_REPAIR_ADD_CMV40_SAFE_DEFAULTS = 1ull << 10,
	DOVI_CAP_REPAIR_REMOVE_CMV40 = 1ull << 11,
	DOVI_CAP_AV1_T35 = 1ull << 12,
	DOVI_CAP_MPV_STATE = 1ull << 13,
} dovi_capability_flags;

typedef enum dovi_inspection_flags {
	DOVI_INSPECTION_NONE = 0,
	DOVI_INSPECTION_MAPPING_PRESENT = 1u << 0,
	DOVI_INSPECTION_CMV40_PRESENT = 1u << 1,
} dovi_inspection_flags;

typedef struct dovi_sample {
	/* Must be sizeof(dovi_sample) for ABI version 3. */
	uint32_t struct_size;
	uint32_t framing;
	uint32_t nal_length_size;
	/* UNKNOWN, HDR10, HDR10_PLUS, or HLG; describes encoded base pixels. */
	uint32_t source_base_presentation;
	const uint8_t* data;
	uint64_t data_size;
	const uint8_t* supplemental_rpu;
	uint64_t supplemental_rpu_size;
} dovi_sample;

typedef struct dovi_transform_request {
	/* Must be sizeof(dovi_transform_request) for ABI version 3. */
	uint32_t struct_size;
	uint32_t target;
	uint32_t repair_flags;
	uint32_t reserved;
} dovi_transform_request;

typedef struct dovi_sample_info {
	uint32_t input_presentation;
	uint32_t rpu_count;
	uint32_t enhancement_nal_count;
	uint32_t video_nal_count;
	uint32_t framing;
	uint32_t nal_length_size;
	uint32_t metadata_flags;
	uint32_t reserved;
} dovi_sample_info;

typedef struct dovi_transform_info {
	uint32_t output_presentation;
	uint32_t applied_repair_flags;
	uint32_t converted_rpu_count;
	uint32_t dropped_dovi_nal_count;
	uint32_t preserved_nal_count;
	uint32_t reserved[3];
} dovi_transform_info;

DOVI_EXPORT uint32_t dovi_abi_version(void);
DOVI_EXPORT uint64_t dovi_capabilities(void);

DOVI_EXPORT dovi_status dovi_inspect_sample(
	const dovi_sample* sample,
	dovi_sample_info* info
);

DOVI_EXPORT dovi_status dovi_transform_sample(
	const dovi_sample* sample,
	const dovi_transform_request* request,
	uint8_t* output,
	uint64_t* output_size,
	dovi_transform_info* info
);

/*
 * output and output_size are caller-owned. On DOVI_OUTPUT_TOO_SMALL,
 * output remains unchanged and output_size receives the required capacity.
 * SOURCE_BASE_PRESENTATION removes RPU/enhancement NALs only; it never changes
 * encoded base pixels or non-Dolby-Vision metadata.
 */

DOVI_EXPORT dovi_status dovi_write_av1_t35(
	const uint8_t* rpu,
	uint64_t rpu_size,
	uint32_t rpu_format,
	uint32_t complete_obu,
	uint8_t* output,
	uint64_t* output_size
);

/* ABI v2 compatibility shims. They intentionally publish no active request. */
DOVI_EXPORT dovi_status dovi_set_mpv_request(const dovi_transform_request* request);
DOVI_EXPORT uint32_t dovi_get_mpv_request(dovi_transform_request* request);
DOVI_EXPORT void dovi_reset_mpv_error(void);
DOVI_EXPORT void dovi_record_mpv_error(dovi_status status);
DOVI_EXPORT dovi_status dovi_consume_mpv_error(void);

DOVI_EXPORT dovi_status dovi_set_mpv_request_v3(
	const dovi_transform_request* request,
	uint64_t* generation
);
/*
 * Publishing, including a null request, returns a new nonzero generation.
 * MPV must snapshot the request and generation together, then use that same
 * generation for reset, record, and consume. Stale generations are ignored.
 */
DOVI_EXPORT uint32_t dovi_get_mpv_request_v3(
	dovi_transform_request* request,
	uint64_t* generation
);
DOVI_EXPORT void dovi_reset_mpv_error_v3(uint64_t generation);
DOVI_EXPORT void dovi_record_mpv_error_v3(uint64_t generation, dovi_status status);
DOVI_EXPORT dovi_status dovi_consume_mpv_error_v3(uint64_t generation);

#ifdef __cplusplus
}

static_assert(sizeof(dovi_transform_request) == 16, "stable request layout changed");
static_assert(sizeof(dovi_status) == sizeof(int32_t), "stable status carrier changed");
static_assert(sizeof(dovi_sample_info) == 32, "stable inspection layout changed");
static_assert(sizeof(dovi_transform_info) == 32, "stable transform info layout changed");
static_assert(DOVI_TARGET_SOURCE_BASE_PRESENTATION == 5, "stable target values changed");
static_assert(DOVI_REPAIR_REMOVE_CMV40 == (1u << 3), "stable repair values changed");
static_assert(DOVI_INSPECTION_CMV40_PRESENT == (1u << 1), "stable inspection flags changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(dovi_transform_request) == 16, "stable request layout changed");
_Static_assert(sizeof(dovi_status) == sizeof(int32_t), "stable status carrier changed");
_Static_assert(sizeof(dovi_sample_info) == 32, "stable inspection layout changed");
_Static_assert(sizeof(dovi_transform_info) == 32, "stable transform info layout changed");
#endif

#endif
