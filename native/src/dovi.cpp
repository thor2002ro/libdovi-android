#include "dovi.h"

#include "libdovi_capi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kKnownRepairFlags =
	DOVI_REPAIR_REMOVE_MAPPING |
	DOVI_REPAIR_ZERO_ACTIVE_AREA |
	DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS |
	DOVI_REPAIR_REMOVE_CMV40;

constexpr uint64_t kCapabilities =
	DOVI_CAP_INSPECT |
	DOVI_CAP_VALIDATE |
	DOVI_CAP_LOSSLESS_REWRITE |
	DOVI_CAP_MEL |
	DOVI_CAP_PROFILE_8_1 |
	DOVI_CAP_PROFILE_8_1_PRESERVE_MAPPING |
	DOVI_CAP_PROFILE_8_4 |
	DOVI_CAP_SOURCE_BASE_PRESENTATION |
	DOVI_CAP_REPAIR_REMOVE_MAPPING |
	DOVI_CAP_REPAIR_ZERO_ACTIVE_AREA |
	DOVI_CAP_REPAIR_ADD_CMV40_SAFE_DEFAULTS |
	DOVI_CAP_REPAIR_REMOVE_CMV40 |
	DOVI_CAP_AV1_T35 |
	DOVI_CAP_MPV_STATE;

static_assert(DOVI_TARGET_MEL == 1);
static_assert(DOVI_TARGET_PROFILE_8_1 == 2);
static_assert(DOVI_TARGET_PROFILE_8_4 == 4);
static_assert(sizeof(uint64_t) == 8);

struct NalUnit {
	std::vector<uint8_t> prefix;
	std::vector<uint8_t> bytes;
};

struct ParsedSample {
	dovi_framing framing = DOVI_FRAMING_AUTO;
	uint8_t nal_length_size = 0;
	std::vector<NalUnit> units;
};

struct RpuDetails {
	uint8_t profile = 0;
	bool mel = false;
	bool mapping_present = false;
	bool cmv40_present = false;
};

struct Inspection {
	ParsedSample parsed;
	RpuDetails rpu;
	dovi_sample_info info{};
};

using RpuPtr = std::unique_ptr<DoviRpuOpaque, decltype(&dovi_rpu_free)>;
using DataPtr = std::unique_ptr<const DoviData, decltype(&dovi_data_free)>;

bool uint64_fits_size(uint64_t value) {
	return value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max());
}

bool is_base_presentation(uint32_t presentation) {
	return presentation == DOVI_PRESENTATION_UNKNOWN ||
		presentation == DOVI_PRESENTATION_HDR10 ||
		presentation == DOVI_PRESENTATION_HDR10_PLUS ||
		presentation == DOVI_PRESENTATION_HLG;
}

dovi_status validate_sample(const dovi_sample* sample) {
	if (sample == nullptr || sample->struct_size != sizeof(dovi_sample) ||
		sample->data == nullptr || sample->data_size == 0 ||
		!uint64_fits_size(sample->data_size) ||
		!uint64_fits_size(sample->supplemental_rpu_size) ||
		(sample->supplemental_rpu == nullptr && sample->supplemental_rpu_size != 0) ||
		!is_base_presentation(sample->source_base_presentation)) {
		return DOVI_INVALID_ARGUMENT;
	}
	if (sample->framing > DOVI_FRAMING_LENGTH_PREFIXED) {
		return DOVI_UNSUPPORTED_FRAMING;
	}
	if (sample->framing == DOVI_FRAMING_LENGTH_PREFIXED &&
		sample->nal_length_size != 1 && sample->nal_length_size != 2 &&
		sample->nal_length_size != 4) {
		return DOVI_UNSUPPORTED_FRAMING;
	}
	return DOVI_OK;
}

dovi_status validate_request(const dovi_transform_request* request) {
	if (request == nullptr || request->struct_size != sizeof(dovi_transform_request) ||
		request->target > DOVI_TARGET_SOURCE_BASE_PRESENTATION ||
		(request->repair_flags & ~kKnownRepairFlags) != 0 || request->reserved != 0) {
		return DOVI_INVALID_ARGUMENT;
	}
	if ((request->repair_flags & DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS) != 0 &&
		(request->repair_flags & DOVI_REPAIR_REMOVE_CMV40) != 0) {
		return DOVI_INVALID_ARGUMENT;
	}
	if (request->target == DOVI_TARGET_SOURCE_BASE_PRESENTATION &&
		request->repair_flags != DOVI_REPAIR_NONE) {
		return DOVI_INVALID_ARGUMENT;
	}
	return DOVI_OK;
}

bool starts_with_start_code(const uint8_t* data, size_t size, size_t* prefix_size) {
	if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
		*prefix_size = 4;
		return true;
	}
	if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
		*prefix_size = 3;
		return true;
	}
	return false;
}

bool find_start_code(
	const uint8_t* data,
	size_t size,
	size_t from,
	size_t* position,
	size_t* prefix_size
) {
	for (size_t index = from; index + 3 <= size; index++) {
		size_t candidate_size = 0;
		if (starts_with_start_code(data + index, size - index, &candidate_size)) {
			*position = index;
			*prefix_size = candidate_size;
			return true;
		}
	}
	return false;
}

dovi_status parse_annex_b(const uint8_t* data, size_t size, std::vector<NalUnit>* units) {
	size_t position = 0;
	size_t prefix_size = 0;
	if (!starts_with_start_code(data, size, &prefix_size)) return DOVI_MALFORMED_SAMPLE;

	while (position < size) {
		if (!starts_with_start_code(data + position, size - position, &prefix_size)) {
			return DOVI_MALFORMED_SAMPLE;
		}
		const size_t nal_start = position + prefix_size;
		size_t next_position = size;
		size_t next_prefix_size = 0;
		find_start_code(data, size, nal_start, &next_position, &next_prefix_size);
		if (next_position <= nal_start || next_position - nal_start < 2) {
			return DOVI_MALFORMED_SAMPLE;
		}
		units->push_back({
			std::vector<uint8_t>(data + position, data + nal_start),
			std::vector<uint8_t>(data + nal_start, data + next_position),
		});
		position = next_position;
	}
	return units->empty() ? DOVI_MALFORMED_SAMPLE : DOVI_OK;
}

size_t read_length(const uint8_t* data, uint8_t length_size) {
	size_t length = 0;
	for (uint8_t index = 0; index < length_size; index++) {
		length = (length << 8) | data[index];
	}
	return length;
}

std::vector<uint8_t> write_length(size_t length, uint8_t length_size) {
	std::vector<uint8_t> bytes(length_size);
	for (uint8_t index = 0; index < length_size; index++) {
		const auto shift = static_cast<unsigned>((length_size - index - 1) * 8);
		bytes[index] = static_cast<uint8_t>((length >> shift) & 0xff);
	}
	return bytes;
}

dovi_status parse_length_prefixed(
	const uint8_t* data,
	size_t size,
	uint8_t length_size,
	std::vector<NalUnit>* units
) {
	if (length_size != 1 && length_size != 2 && length_size != 4) {
		return DOVI_UNSUPPORTED_FRAMING;
	}
	for (size_t position = 0; position < size;) {
		if (size - position < length_size) return DOVI_MALFORMED_SAMPLE;
		const size_t nal_size = read_length(data + position, length_size);
		const size_t nal_start = position + length_size;
		if (nal_size < 2 || nal_size > size - nal_start) return DOVI_MALFORMED_SAMPLE;
		units->push_back({
			std::vector<uint8_t>(data + position, data + nal_start),
			std::vector<uint8_t>(data + nal_start, data + nal_start + nal_size),
		});
		position = nal_start + nal_size;
	}
	return units->empty() ? DOVI_MALFORMED_SAMPLE : DOVI_OK;
}

dovi_status parse_sample(const dovi_sample& sample, ParsedSample* parsed) {
	parsed->framing = static_cast<dovi_framing>(sample.framing);
	parsed->nal_length_size = static_cast<uint8_t>(sample.nal_length_size);
	const auto size = static_cast<size_t>(sample.data_size);
	if (parsed->framing == DOVI_FRAMING_AUTO) {
		size_t prefix_size = 0;
		if (starts_with_start_code(sample.data, size, &prefix_size)) {
			parsed->framing = DOVI_FRAMING_ANNEX_B;
		} else {
			for (uint8_t candidate : std::array<uint8_t, 3>{4, 2, 1}) {
				std::vector<NalUnit> candidate_units;
				if (parse_length_prefixed(sample.data, size, candidate, &candidate_units) == DOVI_OK) {
					parsed->framing = DOVI_FRAMING_LENGTH_PREFIXED;
					parsed->nal_length_size = candidate;
					parsed->units = std::move(candidate_units);
					return DOVI_OK;
				}
			}
			return DOVI_MALFORMED_SAMPLE;
		}
	}
	if (parsed->framing == DOVI_FRAMING_ANNEX_B) {
		return parse_annex_b(sample.data, size, &parsed->units);
	}
	if (parsed->framing == DOVI_FRAMING_LENGTH_PREFIXED) {
		return parse_length_prefixed(sample.data, size, parsed->nal_length_size, &parsed->units);
	}
	return DOVI_UNSUPPORTED_FRAMING;
}

uint8_t nal_type(const std::vector<uint8_t>& nal) {
	return static_cast<uint8_t>((nal[0] >> 1) & 0x3f);
}

uint8_t nal_layer_id(const std::vector<uint8_t>& nal) {
	return static_cast<uint8_t>(((nal[0] & 1) << 5) | (nal[1] >> 3));
}

bool is_enhancement_nal(const std::vector<uint8_t>& nal) {
	return nal_layer_id(nal) > 0 || nal_type(nal) == 63;
}

RpuPtr parse_rpu(const uint8_t* data, size_t size, bool raw) {
	return RpuPtr(
		raw ? dovi_parse_rpu(data, size) : dovi_parse_unspec62_nalu(data, size),
		&dovi_rpu_free);
}

dovi_status parse_supplemental(
	const uint8_t* data,
	size_t size,
	RpuPtr* rpu
) {
	size_t prefix_size = 0;
	if (starts_with_start_code(data, size, &prefix_size)) {
		data += prefix_size;
		size -= prefix_size;
	}
	const bool is_nal = size >= 2 && (((data[0] >> 1) & 0x3f) == 62);
	*rpu = parse_rpu(data, size, !is_nal);
	if (*rpu == nullptr || dovi_rpu_get_error(rpu->get()) != nullptr) {
		return DOVI_RPU_PARSE_FAILED;
	}
	return DOVI_OK;
}

dovi_status inspect_rpu(DoviRpuOpaque* rpu, RpuDetails* details) {
	if (rpu == nullptr || dovi_rpu_get_error(rpu) != nullptr) {
		return DOVI_RPU_PARSE_FAILED;
	}
	const int32_t profile = dovi_rpu_get_profile(rpu);
	const int32_t el_type = dovi_rpu_get_el_type(rpu);
	const int32_t mapping_present = dovi_rpu_has_mapping(rpu);
	const int32_t cmv40_present = dovi_rpu_has_cmv40_metadata(rpu);
	if (profile < 0 || el_type < 0 || mapping_present < 0 || cmv40_present < 0) {
		return DOVI_RPU_PARSE_FAILED;
	}
	details->profile = static_cast<uint8_t>(profile);
	details->mel = el_type == 1;
	details->mapping_present = mapping_present == 1;
	details->cmv40_present = cmv40_present == 1;
	if (details->profile != 5 && details->profile != 7 && details->profile != 8) {
		return DOVI_UNSUPPORTED_PROFILE;
	}
	return DOVI_OK;
}

uint32_t presentation_for(const RpuDetails& details, uint32_t source_base) {
	switch (details.profile) {
	case 5:
		return DOVI_PRESENTATION_PROFILE_5;
	case 7:
		return details.mel ? DOVI_PRESENTATION_PROFILE_7_MEL : DOVI_PRESENTATION_PROFILE_7_FEL;
	case 8:
		if (source_base == DOVI_PRESENTATION_HLG) return DOVI_PRESENTATION_PROFILE_8_4;
		if (source_base == DOVI_PRESENTATION_HDR10 ||
			source_base == DOVI_PRESENTATION_HDR10_PLUS) {
			return DOVI_PRESENTATION_PROFILE_8_1;
		}
		return DOVI_PRESENTATION_UNKNOWN;
	default:
		return DOVI_PRESENTATION_UNKNOWN;
	}
}

dovi_status merge_rpu_details(const RpuDetails& candidate, RpuDetails* common, bool* seen) {
	if (!*seen) {
		*common = candidate;
		*seen = true;
		return DOVI_OK;
	}
	if (common->profile != candidate.profile || common->mel != candidate.mel) {
		return DOVI_INCONSISTENT_RPU;
	}
	common->mapping_present = common->mapping_present || candidate.mapping_present;
	common->cmv40_present = common->cmv40_present || candidate.cmv40_present;
	return DOVI_OK;
}

dovi_status inspect_internal(const dovi_sample& sample, Inspection* inspection) {
	auto status = parse_sample(sample, &inspection->parsed);
	if (status != DOVI_OK) return status;

	bool seen_rpu = false;
	for (const auto& unit : inspection->parsed.units) {
		if (unit.bytes.size() < 2) return DOVI_MALFORMED_SAMPLE;
		if (is_enhancement_nal(unit.bytes)) inspection->info.enhancement_nal_count++;
		if (nal_type(unit.bytes) <= 31) inspection->info.video_nal_count++;
		if (nal_type(unit.bytes) != 62) continue;

		RpuPtr rpu = parse_rpu(unit.bytes.data(), unit.bytes.size(), false);
		RpuDetails details;
		status = inspect_rpu(rpu.get(), &details);
		if (status != DOVI_OK) return status;
		status = merge_rpu_details(details, &inspection->rpu, &seen_rpu);
		if (status != DOVI_OK) return status;
		inspection->info.rpu_count++;
	}

	if (sample.supplemental_rpu_size > 0) {
		RpuPtr rpu(nullptr, &dovi_rpu_free);
		status = parse_supplemental(
			sample.supplemental_rpu,
			static_cast<size_t>(sample.supplemental_rpu_size),
			&rpu);
		if (status != DOVI_OK) return status;
		RpuDetails details;
		status = inspect_rpu(rpu.get(), &details);
		if (status != DOVI_OK) return status;
		status = merge_rpu_details(details, &inspection->rpu, &seen_rpu);
		if (status != DOVI_OK) return status;
		inspection->info.rpu_count++;
	}

	if (!seen_rpu) return DOVI_RPU_NOT_FOUND;
	inspection->info.input_presentation =
		presentation_for(inspection->rpu, sample.source_base_presentation);
	inspection->info.metadata_flags =
		(inspection->rpu.mapping_present ? DOVI_INSPECTION_MAPPING_PRESENT : 0) |
		(inspection->rpu.cmv40_present ? DOVI_INSPECTION_CMV40_PRESENT : 0);
	inspection->info.framing = inspection->parsed.framing;
	inspection->info.nal_length_size = inspection->parsed.nal_length_size;
	return DOVI_OK;
}

dovi_status validate_target(
	const dovi_sample& sample,
	const dovi_transform_request& request,
	const RpuDetails& input
) {
	const uint32_t base = sample.source_base_presentation;
	const bool pq = base == DOVI_PRESENTATION_HDR10 ||
		base == DOVI_PRESENTATION_HDR10_PLUS;
	const bool hlg = base == DOVI_PRESENTATION_HLG;
	switch (request.target) {
	case DOVI_TARGET_LOSSLESS_REWRITE:
		return DOVI_OK;
	case DOVI_TARGET_MEL:
		if (!pq) return DOVI_REENCODE_REQUIRED;
		return input.profile == 7 || input.profile == 8
			? DOVI_OK : DOVI_UNSUPPORTED_PROFILE;
	case DOVI_TARGET_PROFILE_8_1:
		if (!pq) return DOVI_REENCODE_REQUIRED;
		return input.profile == 5 || input.profile == 7 || input.profile == 8
			? DOVI_OK : DOVI_UNSUPPORTED_PROFILE;
	case DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING:
		if (!pq) return DOVI_REENCODE_REQUIRED;
		return input.profile == 7 || input.profile == 8
			? DOVI_OK : DOVI_UNSUPPORTED_PROFILE;
	case DOVI_TARGET_PROFILE_8_4:
		if (!hlg) return DOVI_REENCODE_REQUIRED;
		return input.profile == 8 ? DOVI_OK : DOVI_REENCODE_REQUIRED;
	case DOVI_TARGET_SOURCE_BASE_PRESENTATION:
		return sample.source_base_presentation == DOVI_PRESENTATION_UNKNOWN || input.profile == 5
			? DOVI_REENCODE_REQUIRED : DOVI_OK;
	default:
		return DOVI_INVALID_ARGUMENT;
	}
}

dovi_status convert_rpu(DoviRpuOpaque* rpu, uint32_t target) {
	switch (target) {
	case DOVI_TARGET_LOSSLESS_REWRITE:
		return DOVI_OK;
	case DOVI_TARGET_MEL:
		return dovi_convert_rpu_with_mode(rpu, 1) == 0
			? DOVI_OK : DOVI_RPU_CONVERT_FAILED;
	case DOVI_TARGET_PROFILE_8_1:
		return dovi_convert_rpu_with_mode(rpu, 2) == 0
			? DOVI_OK : DOVI_RPU_CONVERT_FAILED;
	case DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING:
		return dovi_convert_rpu_to_p81_preserve_mapping(rpu) == 0
			? DOVI_OK : DOVI_RPU_CONVERT_FAILED;
	case DOVI_TARGET_PROFILE_8_4:
		return dovi_convert_rpu_with_mode(rpu, 4) == 0
			? DOVI_OK : DOVI_RPU_CONVERT_FAILED;
	default:
		return DOVI_INVALID_ARGUMENT;
	}
}

dovi_status apply_repairs(DoviRpuOpaque* rpu, uint32_t flags) {
	if ((flags & DOVI_REPAIR_REMOVE_MAPPING) != 0 &&
		dovi_rpu_remove_mapping(rpu) != 0) {
		return DOVI_REPAIR_FAILED;
	}
	if ((flags & DOVI_REPAIR_ZERO_ACTIVE_AREA) != 0 &&
		dovi_rpu_set_active_area_offsets(rpu, 0, 0, 0, 0) != 0) {
		return DOVI_REPAIR_FAILED;
	}
	if ((flags & DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS) != 0 &&
		dovi_rpu_add_cmv40_safe_default_metadata(rpu) < 0) {
		return DOVI_REPAIR_FAILED;
	}
	if ((flags & DOVI_REPAIR_REMOVE_CMV40) != 0 &&
		dovi_rpu_remove_cmv40_metadata(rpu) != 0) {
		return DOVI_REPAIR_FAILED;
	}
	return DOVI_OK;
}

dovi_status transform_rpu(
	DoviRpuOpaque* rpu,
	const dovi_transform_request& request,
	std::vector<uint8_t>* output
) {
	auto status = convert_rpu(rpu, request.target);
	if (status != DOVI_OK) return status;
	status = apply_repairs(rpu, request.repair_flags);
	if (status != DOVI_OK) return status;
	DataPtr data(dovi_write_unspec62_nalu(rpu), &dovi_data_free);
	if (data == nullptr || data->data == nullptr || data->len < 2) {
		return DOVI_RPU_WRITE_FAILED;
	}
	output->assign(data->data, data->data + data->len);
	return DOVI_OK;
}

dovi_status transform_rpu_bytes(
	const uint8_t* data,
	size_t size,
	bool raw,
	const dovi_transform_request& request,
	std::vector<uint8_t>* output
) {
	RpuPtr rpu = parse_rpu(data, size, raw);
	if (rpu == nullptr || dovi_rpu_get_error(rpu.get()) != nullptr) {
		return DOVI_RPU_PARSE_FAILED;
	}
	return transform_rpu(rpu.get(), request, output);
}

dovi_status transform_supplemental(
	const uint8_t* data,
	size_t size,
	const dovi_transform_request& request,
	std::vector<uint8_t>* output
) {
	size_t prefix_size = 0;
	if (starts_with_start_code(data, size, &prefix_size)) {
		data += prefix_size;
		size -= prefix_size;
	}
	const bool is_nal = size >= 2 && (((data[0] >> 1) & 0x3f) == 62);
	return transform_rpu_bytes(data, size, !is_nal, request, output);
}

bool length_fits(size_t length, uint8_t length_size) {
	if (length_size == 4) return length <= std::numeric_limits<uint32_t>::max();
	return length < (size_t{1} << (length_size * 8));
}

dovi_status append_unit(
	std::vector<uint8_t>* output,
	const NalUnit& unit,
	dovi_framing framing,
	uint8_t length_size
) {
	if (framing == DOVI_FRAMING_ANNEX_B) {
		output->insert(output->end(), unit.prefix.begin(), unit.prefix.end());
	} else {
		if (!length_fits(unit.bytes.size(), length_size)) return DOVI_INTERNAL_ERROR;
		const auto prefix = write_length(unit.bytes.size(), length_size);
		output->insert(output->end(), prefix.begin(), prefix.end());
	}
	output->insert(output->end(), unit.bytes.begin(), unit.bytes.end());
	return DOVI_OK;
}

uint32_t output_presentation(
	const dovi_sample& sample,
	const dovi_transform_request& request,
	const RpuDetails& input
) {
	switch (request.target) {
	case DOVI_TARGET_LOSSLESS_REWRITE:
		return presentation_for(input, sample.source_base_presentation);
	case DOVI_TARGET_MEL:
		return DOVI_PRESENTATION_PROFILE_7_MEL;
	case DOVI_TARGET_PROFILE_8_1:
	case DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING:
		return DOVI_PRESENTATION_PROFILE_8_1;
	case DOVI_TARGET_PROFILE_8_4:
		return DOVI_PRESENTATION_PROFILE_8_4;
	case DOVI_TARGET_SOURCE_BASE_PRESENTATION:
		return sample.source_base_presentation;
	default:
		return DOVI_PRESENTATION_UNKNOWN;
	}
}

dovi_status build_transformed_sample(
	const dovi_sample& sample,
	const dovi_transform_request& request,
	const Inspection& inspection,
	std::vector<uint8_t>* output,
	dovi_transform_info* info
) {
	std::vector<NalUnit> retained;
	retained.reserve(inspection.parsed.units.size() + (sample.supplemental_rpu_size > 0 ? 1 : 0));
	const bool source_base = request.target == DOVI_TARGET_SOURCE_BASE_PRESENTATION;
	const bool drops_enhancement_data =
		request.target == DOVI_TARGET_MEL ||
		request.target == DOVI_TARGET_PROFILE_8_1 ||
		request.target == DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING ||
		request.target == DOVI_TARGET_PROFILE_8_4;

	for (const auto& input_unit : inspection.parsed.units) {
		const auto type = nal_type(input_unit.bytes);
		if ((source_base && (type == 62 || is_enhancement_nal(input_unit.bytes))) ||
			(drops_enhancement_data && is_enhancement_nal(input_unit.bytes))) {
			info->dropped_dovi_nal_count++;
			continue;
		}

		NalUnit unit = input_unit;
		if (type == 62) {
			auto status = transform_rpu_bytes(
				unit.bytes.data(), unit.bytes.size(), false, request, &unit.bytes);
			if (status != DOVI_OK) return status;
			info->converted_rpu_count++;
		} else {
			info->preserved_nal_count++;
		}
		retained.push_back(std::move(unit));
	}

	if (sample.supplemental_rpu_size > 0 && source_base) {
		info->dropped_dovi_nal_count++;
	} else if (sample.supplemental_rpu_size > 0) {
		std::vector<uint8_t> converted;
		auto status = transform_supplemental(
			sample.supplemental_rpu,
			static_cast<size_t>(sample.supplemental_rpu_size),
			request,
			&converted);
		if (status != DOVI_OK) return status;
		auto insertion = std::find_if(retained.begin(), retained.end(), [](const NalUnit& unit) {
			return nal_type(unit.bytes) <= 31;
		});
		std::vector<uint8_t> prefix;
		if (inspection.parsed.framing == DOVI_FRAMING_ANNEX_B) {
			prefix = insertion == retained.end()
				? std::vector<uint8_t>{0, 0, 0, 1}
				: insertion->prefix;
		}
		retained.insert(insertion, NalUnit{std::move(prefix), std::move(converted)});
		info->converted_rpu_count++;
	}

	for (const auto& unit : retained) {
		auto status = append_unit(
			output, unit, inspection.parsed.framing, inspection.parsed.nal_length_size);
		if (status != DOVI_OK) return status;
	}
	return DOVI_OK;
}

dovi_status copy_output(
	const std::vector<uint8_t>& bytes,
	uint8_t* output,
	uint64_t* output_size
) {
	if (output_size == nullptr) return DOVI_INVALID_ARGUMENT;
	const uint64_t capacity = *output_size;
	*output_size = static_cast<uint64_t>(bytes.size());
	if (capacity < bytes.size() || (output == nullptr && !bytes.empty())) {
		return DOVI_OUTPUT_TOO_SMALL;
	}
	if (!bytes.empty()) std::memcpy(output, bytes.data(), bytes.size());
	return DOVI_OK;
}

} // namespace

extern "C" {

uint32_t dovi_abi_version(void) {
	return DOVI_ABI_VERSION;
}

uint64_t dovi_capabilities(void) {
	return kCapabilities;
}

dovi_status dovi_inspect_sample(
	const dovi_sample* sample,
	dovi_sample_info* info
) {
	try {
		if (info == nullptr) return DOVI_INVALID_ARGUMENT;
		auto status = validate_sample(sample);
		if (status != DOVI_OK) return status;
		Inspection inspection;
		status = inspect_internal(*sample, &inspection);
		if (status == DOVI_OK) *info = inspection.info;
		return status;
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

dovi_status dovi_transform_sample(
	const dovi_sample* sample,
	const dovi_transform_request* request,
	uint8_t* output,
	uint64_t* output_size,
	dovi_transform_info* info
) {
	try {
		if (output_size == nullptr) return DOVI_INVALID_ARGUMENT;
		dovi_transform_info result_info{};
		auto status = validate_sample(sample);
		if (status != DOVI_OK) return status;
		status = validate_request(request);
		if (status != DOVI_OK) return status;

		Inspection inspection;
		status = inspect_internal(*sample, &inspection);
		if (status != DOVI_OK) return status;
		status = validate_target(*sample, *request, inspection.rpu);
		if (status != DOVI_OK) return status;

		result_info.output_presentation = output_presentation(*sample, *request, inspection.rpu);
		result_info.applied_repair_flags = request->repair_flags;
		std::vector<uint8_t> transformed;
		status = build_transformed_sample(
			*sample, *request, inspection, &transformed, &result_info);
		if (status != DOVI_OK) return status;
		const auto copy_status = copy_output(transformed, output, output_size);
		if ((copy_status == DOVI_OK || copy_status == DOVI_OUTPUT_TOO_SMALL) && info != nullptr) {
			*info = result_info;
		}
		return copy_status;
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

dovi_status dovi_write_av1_t35(
	const uint8_t* rpu,
	uint64_t rpu_size,
	uint32_t rpu_format,
	uint32_t complete_obu,
	uint8_t* output,
	uint64_t* output_size
) {
	try {
		if (rpu == nullptr || rpu_size == 0 || !uint64_fits_size(rpu_size) ||
			output_size == nullptr || rpu_format > DOVI_RPU_FORMAT_UNSPEC62_NAL ||
			complete_obu > 1) {
			return DOVI_INVALID_ARGUMENT;
		}
		RpuPtr parsed = parse_rpu(
			rpu,
			static_cast<size_t>(rpu_size),
			rpu_format == DOVI_RPU_FORMAT_RAW);
		if (parsed == nullptr || dovi_rpu_get_error(parsed.get()) != nullptr) {
			return DOVI_RPU_PARSE_FAILED;
		}
		DataPtr data(
			complete_obu != 0
				? dovi_write_av1_rpu_metadata_obu_t35_complete(parsed.get())
				: dovi_write_av1_rpu_metadata_obu_t35_payload(parsed.get()),
			&dovi_data_free);
		if (data == nullptr || data->data == nullptr) return DOVI_RPU_WRITE_FAILED;
		const std::vector<uint8_t> bytes(data->data, data->data + data->len);
		return copy_output(bytes, output, output_size);
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

} // extern "C"
