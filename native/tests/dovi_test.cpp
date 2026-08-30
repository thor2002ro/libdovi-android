#include "dovi.h"
#include "../src/libdovi_capi.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <malloc.h>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if defined(DOVI_REAL_LIBDOVI)

namespace {

std::vector<uint8_t> read_file(const char* path) {
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> fixture_nal(const std::vector<uint8_t>& fixture) {
	std::vector<uint8_t> nal{0x7c, 0x01};
	nal.insert(nal.end(), fixture.begin() + 4, fixture.end());
	return nal;
}

std::vector<uint8_t> annex_unit(const std::vector<uint8_t>& nal) {
	std::vector<uint8_t> bytes{0, 0, 0, 1};
	bytes.insert(bytes.end(), nal.begin(), nal.end());
	return bytes;
}

bool transform_fixture(
	const dovi_sample& sample,
	const dovi_transform_request& request,
	std::vector<uint8_t>* output,
	dovi_transform_info* info
) {
	uint64_t output_size = 0;
	if (dovi_transform_sample(
		&sample, &request, nullptr, &output_size, info) != DOVI_OUTPUT_TOO_SMALL) {
		return false;
	}
	output->resize(output_size);
	return dovi_transform_sample(
		&sample, &request, output->data(), &output_size, info) == DOVI_OK;
}

} // namespace

int main(int argc, char** argv) {
	if (argc != 5) {
		std::cerr << "usage: dovi_test P7_INPUT EXPECTED_P81 P84_INPUT EXPECTED_P84\n";
		return 2;
	}
	const auto p7_fixture = read_file(argv[1]);
	const auto p81_fixture = read_file(argv[2]);
	const auto p84_fixture = read_file(argv[3]);
	const auto expected_p84_fixture = read_file(argv[4]);
	if (p7_fixture.size() <= 4 || p81_fixture.size() <= 4 ||
		p84_fixture.size() <= 4 || expected_p84_fixture.size() <= 4) {
		std::cerr << "fixture input is empty\n";
		return 2;
	}

	const auto p7_nal = fixture_nal(p7_fixture);
	const auto p7_input = annex_unit(p7_nal);
	const auto expected_p81 = annex_unit(fixture_nal(p81_fixture));
	const dovi_sample p7_sample{
		sizeof(dovi_sample),
		DOVI_FRAMING_ANNEX_B,
		0,
		DOVI_PRESENTATION_HDR10,
		p7_input.data(),
		p7_input.size(),
		nullptr,
		0,
	};
	dovi_sample_info inspection{};
	if (dovi_inspect_sample(&p7_sample, &inspection) != DOVI_OK ||
		(inspection.input_presentation != DOVI_PRESENTATION_PROFILE_7_MEL &&
		 inspection.input_presentation != DOVI_PRESENTATION_PROFILE_7_FEL)) {
		std::cerr << "scalar getter inspection failed\n";
		return 1;
	}

	const dovi_transform_request p81_request{
		sizeof(dovi_transform_request),
		DOVI_TARGET_PROFILE_8_1,
		DOVI_REPAIR_NONE,
		0,
	};
	dovi_transform_info info{};
	std::vector<uint8_t> output;
	if (!transform_fixture(p7_sample, p81_request, &output, &info)) {
		std::cerr << "fixture transform failed\n";
		return 1;
	}
	if (output != expected_p81 || info.output_presentation != DOVI_PRESENTATION_PROFILE_8_1 ||
		info.input_presentation != inspection.input_presentation ||
		info.converted_rpu_count != 1) {
		std::cerr << "fixture differs from official P8.1 output\n";
		return 1;
	}

	const auto p84_input = annex_unit(fixture_nal(p84_fixture));
	const auto expected_p84 = annex_unit(fixture_nal(expected_p84_fixture));
	const dovi_sample p84_sample{
		sizeof(dovi_sample), DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HLG,
		p84_input.data(), p84_input.size(), nullptr, 0,
	};
	const dovi_transform_request p84_request{
		sizeof(dovi_transform_request), DOVI_TARGET_PROFILE_8_4, DOVI_REPAIR_NONE, 0,
	};
	if (!transform_fixture(p84_sample, p84_request, &output, &info) || output != expected_p84 ||
		info.output_presentation != DOVI_PRESENTATION_PROFILE_8_4) {
		std::cerr << "fixture differs from official P8.4 output\n";
		return 1;
	}

	for (uint32_t repair : {
		uint32_t{DOVI_REPAIR_REMOVE_MAPPING},
		uint32_t{DOVI_REPAIR_ZERO_ACTIVE_AREA},
		uint32_t{DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS},
		uint32_t{DOVI_REPAIR_REMOVE_CMV40},
	}) {
		const dovi_transform_request repair_request{
			sizeof(dovi_transform_request), DOVI_TARGET_LOSSLESS_REWRITE, repair, 0,
		};
		if (!transform_fixture(p7_sample, repair_request, &output, &info) ||
			info.applied_repair_flags != repair) {
			std::cerr << "real repair fixture failed\n";
			return 1;
		}
	}

	uint64_t av1_payload_size = 0;
	uint64_t av1_complete_size = 0;
	if (dovi_write_av1_t35(
		p7_nal.data(), p7_nal.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
		0, nullptr, &av1_payload_size) != DOVI_OUTPUT_TOO_SMALL ||
		dovi_write_av1_t35(
		p7_nal.data(), p7_nal.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
		1, nullptr, &av1_complete_size) != DOVI_OUTPUT_TOO_SMALL) {
		std::cerr << "real AV1 capacity query failed\n";
		return 1;
	}
	std::vector<uint8_t> av1_payload(av1_payload_size);
	std::vector<uint8_t> av1_complete(av1_complete_size);
	if (dovi_write_av1_t35(
		p7_nal.data(), p7_nal.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
		0, av1_payload.data(), &av1_payload_size) != DOVI_OK ||
		dovi_write_av1_t35(
		p7_nal.data(), p7_nal.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
		1, av1_complete.data(), &av1_complete_size) != DOVI_OK ||
		av1_complete.empty() || av1_complete.front() != 0xB5 ||
		av1_complete.size() != av1_payload.size() + 1 ||
		!std::equal(av1_payload.begin(), av1_payload.end(), av1_complete.begin() + 1)) {
		std::cerr << "official AV1 T.35 structure failed\n";
		return 1;
	}

	std::vector<uint8_t> source_base_input = p7_input;
	const std::vector<uint8_t> enhancement{0, 0, 0, 1, 0x02, 0x09, 0x30};
	const std::vector<uint8_t> hdr10_plus{0, 0, 0, 1, 0x4e, 0x01, 0xb5, 0x3c};
	const std::vector<uint8_t> vcl{0, 0, 0, 1, 0x02, 0x01, 0x50};
	source_base_input.insert(source_base_input.end(), enhancement.begin(), enhancement.end());
	source_base_input.insert(source_base_input.end(), hdr10_plus.begin(), hdr10_plus.end());
	source_base_input.insert(source_base_input.end(), vcl.begin(), vcl.end());
	const dovi_sample source_base_sample{
		sizeof(dovi_sample), DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HDR10_PLUS,
		source_base_input.data(), source_base_input.size(), nullptr, 0,
	};
	const dovi_transform_request source_base_request{
		sizeof(dovi_transform_request), DOVI_TARGET_SOURCE_BASE_PRESENTATION,
		DOVI_REPAIR_NONE, 0,
	};
	std::vector<uint8_t> expected_source_base = hdr10_plus;
	expected_source_base.insert(expected_source_base.end(), vcl.begin(), vcl.end());
	if (!transform_fixture(source_base_sample, source_base_request, &output, &info) ||
		output != expected_source_base ||
		info.output_presentation != DOVI_PRESENTATION_HDR10_PLUS) {
		std::cerr << "real source-base stripping failed\n";
		return 1;
	}

	std::cout << "Official libdovi fixture matrix passed\n";
	return 0;
}

#else

namespace {

size_t allocation_count = 0;
bool count_allocations = false;

} // namespace

void* operator new(size_t size) {
	if (count_allocations) allocation_count++;
	if (void* memory = std::malloc(size)) return memory;
	throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
	std::free(memory);
}

void operator delete(void* memory, size_t) noexcept {
	std::free(memory);
}

namespace {

struct FakeRpu {
	std::vector<uint8_t> bytes;
	std::string error;
	uint8_t profile = 7;
	uint8_t failure_code = 0;
	std::string el_type = "FEL";
	uint8_t conversion = 0;
	uint32_t repairs = 0;
	bool fail_write = false;
};

int failures = 0;
std::vector<uint8_t> observed_conversion_modes;
uint32_t preserve_mapping_calls = 0;
uint32_t parse_calls = 0;

#define EXPECT_TRUE(value) expect((value), #value, __LINE__)
#define EXPECT_EQ(actual, expected) expect_equal((actual), (expected), #actual, #expected, __LINE__)

void expect(bool value, const char* expression, int line) {
	if (!value) {
		std::cerr << "FAIL line " << line << ": " << expression << '\n';
		failures++;
	}
}

template<typename A, typename E>
void expect_equal(const A& actual, const E& expected, const char* actual_name, const char* expected_name, int line) {
	if (!(actual == expected)) {
		std::cerr << "FAIL line " << line << ": " << actual_name << " != " << expected_name << '\n';
		failures++;
	}
}

std::vector<uint8_t> nal(uint8_t type, uint8_t layer, std::initializer_list<uint8_t> payload) {
	std::vector<uint8_t> bytes{
		static_cast<uint8_t>((type << 1) | ((layer >> 5) & 1)),
		static_cast<uint8_t>(((layer & 0x1f) << 3) | 1),
	};
	bytes.insert(bytes.end(), payload);
	return bytes;
}

std::vector<uint8_t> rpu(uint8_t profile, uint8_t el = 2) {
	return nal(62, 0, {profile, el});
}

std::vector<uint8_t> annex_b(const std::vector<std::vector<uint8_t>>& units) {
	std::vector<uint8_t> bytes;
	for (size_t index = 0; index < units.size(); index++) {
		const std::vector<uint8_t> prefix = index % 2 == 0
			? std::vector<uint8_t>{0, 0, 0, 1}
			: std::vector<uint8_t>{0, 0, 1};
		bytes.insert(bytes.end(), prefix.begin(), prefix.end());
		bytes.insert(bytes.end(), units[index].begin(), units[index].end());
	}
	return bytes;
}

std::vector<uint8_t> length_prefixed(
	uint8_t length_size,
	const std::vector<std::vector<uint8_t>>& units
) {
	std::vector<uint8_t> bytes;
	for (const auto& unit : units) {
		for (int shift = (length_size - 1) * 8; shift >= 0; shift -= 8) {
			bytes.push_back(static_cast<uint8_t>((unit.size() >> shift) & 0xff));
		}
		bytes.insert(bytes.end(), unit.begin(), unit.end());
	}
	return bytes;
}

dovi_sample sample_of(
	const std::vector<uint8_t>& bytes,
	dovi_framing framing,
	uint32_t length_size = 0,
	dovi_presentation base = DOVI_PRESENTATION_HDR10,
	const std::vector<uint8_t>* supplemental = nullptr
) {
	return {
		sizeof(dovi_sample),
		static_cast<uint32_t>(framing),
		length_size,
		static_cast<uint32_t>(base),
		bytes.data(),
		static_cast<uint64_t>(bytes.size()),
		supplemental == nullptr ? nullptr : supplemental->data(),
		supplemental == nullptr ? 0u : static_cast<uint64_t>(supplemental->size()),
	};
}

dovi_transform_request request_of(dovi_target target, uint32_t repairs = 0) {
	return {
		sizeof(dovi_transform_request),
		static_cast<uint32_t>(target),
		repairs,
		0,
	};
}

struct TransformResult {
	dovi_status status;
	std::vector<uint8_t> bytes;
	dovi_transform_info info{};
};

TransformResult transform(
	const dovi_sample& sample,
	const dovi_transform_request& request
) {
	TransformResult result{};
	dovi_owned_buffer output{};
	result.status = dovi_transform_sample_alloc(
		&sample, &request, &output, &result.info);
	if (result.status == DOVI_OK) {
		result.bytes.assign(output.data, output.data + output.size);
	}
	dovi_owned_buffer_free(&output);
	return result;
}

void test_abi_and_capabilities_are_semantic() {
	EXPECT_EQ(dovi_abi_version(), DOVI_ABI_VERSION);
	const auto capabilities = dovi_capabilities();
	EXPECT_TRUE((capabilities & DOVI_CAP_PROFILE_8_1_PRESERVE_MAPPING) != 0);
	EXPECT_TRUE((capabilities & DOVI_CAP_AV1_T35) != 0);
	EXPECT_TRUE((capabilities & DOVI_CAP_MPV_STATE) != 0);
}

void test_inspection_supports_all_framings_and_supplemental_rpu() {
	for (uint8_t length_size : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
		const auto bytes = length_prefixed(length_size, {rpu(7, 1), nal(1, 0, {0x33})});
		const auto sample = sample_of(bytes, DOVI_FRAMING_LENGTH_PREFIXED, length_size);
		dovi_sample_info info{};
		EXPECT_EQ(dovi_inspect_sample(&sample, &info), DOVI_OK);
		EXPECT_EQ(info.input_presentation, DOVI_PRESENTATION_PROFILE_7_MEL);
		EXPECT_EQ(info.rpu_count, 1u);
		EXPECT_EQ(info.video_nal_count, 1u);
		EXPECT_EQ(info.nal_length_size, static_cast<uint32_t>(length_size));
		EXPECT_TRUE((info.metadata_flags & DOVI_INSPECTION_MAPPING_PRESENT) != 0);
		EXPECT_TRUE((info.metadata_flags & DOVI_INSPECTION_CMV40_PRESENT) == 0);
		const auto transformed = transform(
			sample, request_of(DOVI_TARGET_PROFILE_8_1));
		EXPECT_EQ(transformed.status, DOVI_OK);
		EXPECT_EQ(transformed.bytes, length_prefixed(
			length_size, {rpu(8), nal(1, 0, {0x33})}));
	}

	const auto bytes = annex_b({nal(32, 0, {0x10}), nal(1, 0, {0x50})});
	const auto supplemental = rpu(8);
	const auto sample = sample_of(
		bytes, DOVI_FRAMING_AUTO, 0, DOVI_PRESENTATION_HLG, &supplemental);
	dovi_sample_info info{};
	EXPECT_EQ(dovi_inspect_sample(&sample, &info), DOVI_OK);
	EXPECT_EQ(info.input_presentation, DOVI_PRESENTATION_PROFILE_8_4);
	EXPECT_EQ(info.rpu_count, 1u);
	EXPECT_EQ(info.framing, DOVI_FRAMING_ANNEX_B);
}

void test_inspection_rejects_malformed_and_inconsistent_rpus() {
	const std::vector<uint8_t> malformed{0, 0, 0, 8, 0x7c, 0x01};
	const auto malformed_sample = sample_of(malformed, DOVI_FRAMING_LENGTH_PREFIXED, 4);
	dovi_sample_info info{};
	EXPECT_EQ(dovi_inspect_sample(&malformed_sample, &info), DOVI_MALFORMED_SAMPLE);

	const auto bad_rpu = annex_b({rpu(0xee)});
	const auto bad_sample = sample_of(bad_rpu, DOVI_FRAMING_ANNEX_B);
	EXPECT_EQ(dovi_inspect_sample(&bad_sample, &info), DOVI_RPU_PARSE_FAILED);

	const auto mixed = annex_b({rpu(7), rpu(8)});
	const auto mixed_sample = sample_of(mixed, DOVI_FRAMING_ANNEX_B);
	EXPECT_EQ(dovi_inspect_sample(&mixed_sample, &info), DOVI_INCONSISTENT_RPU);
}

void test_lossless_rewrite_preserves_non_rpu_units() {
	observed_conversion_modes.clear();
	const auto bytes = annex_b({nal(32, 0, {0x10}), rpu(7), nal(1, 1, {0x30}), nal(1, 0, {0x50})});
	const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
	const auto request = request_of(DOVI_TARGET_LOSSLESS_REWRITE);
	const auto result = transform(sample, request);
	EXPECT_EQ(result.status, DOVI_OK);
	EXPECT_EQ(result.bytes, bytes);
	EXPECT_EQ(result.info.input_presentation, DOVI_PRESENTATION_PROFILE_7_FEL);
	EXPECT_EQ(result.info.output_presentation, DOVI_PRESENTATION_PROFILE_7_FEL);
	EXPECT_EQ(result.info.dropped_dovi_nal_count, 0u);
	EXPECT_TRUE(observed_conversion_modes.empty());
}

void test_owned_transform_converts_once_and_releases_output() {
	const auto base = nal(1, 0, {0x50});
	const auto enhancement = nal(1, 1, {0x30});
	const auto bytes = annex_b({rpu(7, 2), enhancement, base});
	const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
	const auto request = request_of(DOVI_TARGET_PROFILE_8_1);
	dovi_owned_buffer output{};
	dovi_transform_info info{};
	observed_conversion_modes.clear();
	parse_calls = 0;

	EXPECT_EQ(
		dovi_transform_sample_alloc(&sample, &request, &output, &info),
		DOVI_OK);
	EXPECT_EQ(observed_conversion_modes, std::vector<uint8_t>{2});
	EXPECT_EQ(parse_calls, 1u);
	EXPECT_EQ(info.input_presentation, DOVI_PRESENTATION_PROFILE_7_FEL);
	EXPECT_EQ(info.output_presentation, DOVI_PRESENTATION_PROFILE_8_1);
	EXPECT_EQ(info.dropped_dovi_nal_count, 1u);
	auto expected = std::vector<uint8_t>{0, 0, 0, 1};
	const auto converted_rpu = rpu(8, 2);
	expected.insert(expected.end(), converted_rpu.begin(), converted_rpu.end());
	expected.insert(expected.end(), {0, 0, 0, 1});
	expected.insert(expected.end(), base.begin(), base.end());
	EXPECT_EQ(
		std::vector<uint8_t>(output.data, output.data + output.size),
		expected);
	constexpr size_t required_padding = DOVI_OUTPUT_PADDING_SIZE;
	const auto allocation_size = malloc_usable_size(output.data);
	EXPECT_TRUE(allocation_size >= output.size + required_padding);
	if (allocation_size >= output.size + required_padding) {
		for (size_t index = 0; index < required_padding; index++) {
			EXPECT_EQ(output.data[output.size + index], 0u);
		}
	}

	dovi_owned_buffer_free(&output);
	EXPECT_EQ(output.data, nullptr);
	EXPECT_EQ(output.size, 0u);
}

void test_mel_and_profile81_targets_cover_supported_profiles() {
	const auto base = nal(1, 0, {0x50});
	const auto enhancement = nal(1, 1, {0x30});
	const auto type63 = nal(63, 0, {0x40});
	const auto p7_bytes = annex_b({rpu(7, 2), enhancement, type63, base});
	const auto p7_sample = sample_of(p7_bytes, DOVI_FRAMING_ANNEX_B);
	observed_conversion_modes.clear();
	const auto mel = transform(p7_sample, request_of(DOVI_TARGET_MEL));
	EXPECT_EQ(mel.status, DOVI_OK);
	EXPECT_EQ(mel.info.output_presentation, DOVI_PRESENTATION_PROFILE_7_MEL);
	EXPECT_EQ(mel.info.dropped_dovi_nal_count, 2u);
	EXPECT_EQ(mel.info.preserved_nal_count, 1u);
	EXPECT_EQ(observed_conversion_modes, std::vector<uint8_t>{1});
	auto expected_mel = std::vector<uint8_t>{0, 0, 0, 1};
	const auto converted_mel_rpu = rpu(7, 1);
	expected_mel.insert(expected_mel.end(), converted_mel_rpu.begin(), converted_mel_rpu.end());
	expected_mel.insert(expected_mel.end(), {0, 0, 1});
	expected_mel.insert(expected_mel.end(), base.begin(), base.end());
	EXPECT_EQ(mel.bytes, expected_mel);

	for (uint8_t profile : {uint8_t{5}, uint8_t{7}, uint8_t{8}}) {
		observed_conversion_modes.clear();
		const auto bytes = annex_b({rpu(profile), nal(1, 0, {0x50})});
		const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
		const auto result = transform(sample, request_of(DOVI_TARGET_PROFILE_8_1));
		EXPECT_EQ(result.status, DOVI_OK);
		EXPECT_EQ(result.info.output_presentation, DOVI_PRESENTATION_PROFILE_8_1);
		EXPECT_EQ(observed_conversion_modes, std::vector<uint8_t>{2});
	}

	observed_conversion_modes.clear();
	preserve_mapping_calls = 0;
	const auto preserved = transform(
		p7_sample, request_of(DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING));
	EXPECT_EQ(preserved.status, DOVI_OK);
	EXPECT_EQ(preserved.info.output_presentation, DOVI_PRESENTATION_PROFILE_8_1);
	auto preserved_expected = std::vector<uint8_t>{0, 0, 0, 1};
	const auto preserved_rpu = nal(62, 0, {8, 3});
	const auto preserved_vcl = nal(1, 0, {0x50});
	preserved_expected.insert(preserved_expected.end(), preserved_rpu.begin(), preserved_rpu.end());
	preserved_expected.insert(preserved_expected.end(), {0, 0, 1});
	preserved_expected.insert(preserved_expected.end(), preserved_vcl.begin(), preserved_vcl.end());
	EXPECT_EQ(preserved.bytes, preserved_expected);
	EXPECT_TRUE(observed_conversion_modes.empty());
	EXPECT_EQ(preserve_mapping_calls, 1u);
}

void test_profile84_requires_an_hlg_encoded_base() {
	const auto bytes = annex_b({rpu(8), nal(1, 0, {0x50})});
	const auto pq_sample = sample_of(
		bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HDR10);
	const auto rejected = transform(pq_sample, request_of(DOVI_TARGET_PROFILE_8_4));
	EXPECT_EQ(rejected.status, DOVI_REENCODE_REQUIRED);

	const auto hlg_sample = sample_of(
		bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HLG);
	observed_conversion_modes.clear();
	const auto accepted = transform(hlg_sample, request_of(DOVI_TARGET_PROFILE_8_4));
	EXPECT_EQ(accepted.status, DOVI_OK);
	EXPECT_EQ(accepted.info.output_presentation, DOVI_PRESENTATION_PROFILE_8_4);
	EXPECT_EQ(observed_conversion_modes, std::vector<uint8_t>{4});
	const auto hlg_to_pq = transform(hlg_sample, request_of(DOVI_TARGET_PROFILE_8_1));
	EXPECT_EQ(hlg_to_pq.status, DOVI_REENCODE_REQUIRED);

	const auto p5_bytes = annex_b({rpu(5), nal(1, 0, {0x50})});
	const auto p5_sample = sample_of(
		p5_bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HDR10);
	const auto p5_base = transform(
		p5_sample, request_of(DOVI_TARGET_SOURCE_BASE_PRESENTATION));
	EXPECT_EQ(p5_base.status, DOVI_REENCODE_REQUIRED);

	const auto unknown_sample = sample_of(
		bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_UNKNOWN);
	for (dovi_target target : {
		DOVI_TARGET_MEL,
		DOVI_TARGET_PROFILE_8_1,
		DOVI_TARGET_PROFILE_8_1_PRESERVE_MAPPING,
		DOVI_TARGET_PROFILE_8_4,
	}) {
		const auto unknown_result = transform(unknown_sample, request_of(target));
		EXPECT_EQ(unknown_result.status, DOVI_REENCODE_REQUIRED);
	}
}

void test_source_base_drops_only_dolby_vision_data_and_preserves_hdr10_plus() {
	const auto hdr10_plus_sei = nal(39, 0, {0xb5, 0x00, 0x3c});
	const auto vcl = nal(1, 0, {0x50});
	const auto bytes = annex_b({hdr10_plus_sei, rpu(7), nal(1, 1, {0x30}), nal(63, 0, {0x40}), vcl});
	const std::vector<uint8_t> supplemental{7, 2};
	const auto sample = sample_of(
		bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HDR10_PLUS, &supplemental);
	const auto result = transform(sample, request_of(DOVI_TARGET_SOURCE_BASE_PRESENTATION));
	EXPECT_EQ(result.status, DOVI_OK);
	EXPECT_EQ(result.info.output_presentation, DOVI_PRESENTATION_HDR10_PLUS);
	auto expected = std::vector<uint8_t>{0, 0, 0, 1};
	expected.insert(expected.end(), hdr10_plus_sei.begin(), hdr10_plus_sei.end());
	expected.insert(expected.end(), {0, 0, 0, 1});
	expected.insert(expected.end(), vcl.begin(), vcl.end());
	EXPECT_EQ(result.bytes, expected);
	EXPECT_EQ(result.info.dropped_dovi_nal_count, 4u);
}

size_t source_base_transform_allocations(size_t video_nal_count) {
	std::vector<std::vector<uint8_t>> units;
	units.reserve(video_nal_count + 1);
	units.push_back(rpu(7));
	for (size_t index = 0; index < video_nal_count; index++) {
		units.push_back(nal(1, 0, {0x50}));
	}
	const auto bytes = annex_b(units);
	const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
	const auto request = request_of(DOVI_TARGET_SOURCE_BASE_PRESENTATION);
	std::vector<uint8_t> output(bytes.size());
	uint64_t output_size = output.size();
	dovi_transform_info info{};

	allocation_count = 0;
	count_allocations = true;
	const auto status = dovi_transform_sample(
		&sample, &request, output.data(), &output_size, &info);
	count_allocations = false;
	EXPECT_EQ(status, DOVI_OK);
	return allocation_count;
}

void test_preserved_nals_do_not_add_heap_allocations_per_nal() {
	const auto small = source_base_transform_allocations(1);
	const auto large = source_base_transform_allocations(128);

	EXPECT_TRUE(large <= small + 16);
}

void test_repairs_apply_independently_and_reject_conflicts() {
	const auto bytes = annex_b({rpu(7)});
	const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
	for (uint32_t repair : {
		uint32_t{DOVI_REPAIR_REMOVE_MAPPING},
		uint32_t{DOVI_REPAIR_ZERO_ACTIVE_AREA},
		uint32_t{DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS},
		uint32_t{DOVI_REPAIR_REMOVE_CMV40},
	}) {
		const auto result = transform(
			sample, request_of(DOVI_TARGET_LOSSLESS_REWRITE, repair));
		EXPECT_EQ(result.status, DOVI_OK);
		EXPECT_EQ(result.info.applied_repair_flags, repair);
		EXPECT_EQ(result.bytes.back(), static_cast<uint8_t>(repair));
	}
	const uint32_t combined_repairs =
		DOVI_REPAIR_REMOVE_MAPPING | DOVI_REPAIR_ZERO_ACTIVE_AREA;
	const auto combined = transform(
		sample, request_of(DOVI_TARGET_LOSSLESS_REWRITE, combined_repairs));
	EXPECT_EQ(combined.status, DOVI_OK);
	EXPECT_EQ(combined.info.applied_repair_flags, combined_repairs);
	EXPECT_EQ(combined.bytes.back(), static_cast<uint8_t>(combined_repairs));

	const auto conflict = transform(sample, request_of(
		DOVI_TARGET_LOSSLESS_REWRITE,
		DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS | DOVI_REPAIR_REMOVE_CMV40));
	EXPECT_EQ(conflict.status, DOVI_INVALID_ARGUMENT);
}

void test_supplemental_rpu_is_inserted_before_the_first_vcl() {
	const auto bytes = annex_b({nal(32, 0, {0x10}), nal(1, 0, {0x50})});
	const std::vector<uint8_t> supplemental{7, 2};
	const auto sample = sample_of(
		bytes, DOVI_FRAMING_ANNEX_B, 0, DOVI_PRESENTATION_HDR10, &supplemental);
	const auto result = transform(sample, request_of(DOVI_TARGET_PROFILE_8_1));
	EXPECT_EQ(result.status, DOVI_OK);
	EXPECT_EQ(result.info.converted_rpu_count, 1u);
	auto expected = std::vector<uint8_t>{0, 0, 0, 1};
	const auto vps = nal(32, 0, {0x10});
	const auto converted_rpu = rpu(8);
	const auto vcl = nal(1, 0, {0x50});
	expected.insert(expected.end(), vps.begin(), vps.end());
	expected.insert(expected.end(), {0, 0, 1});
	expected.insert(expected.end(), converted_rpu.begin(), converted_rpu.end());
	expected.insert(expected.end(), {0, 0, 1});
	expected.insert(expected.end(), vcl.begin(), vcl.end());
	EXPECT_EQ(result.bytes, expected);
}

void test_capacity_negotiation_is_atomic() {
	const auto bytes = annex_b({rpu(7)});
	const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
	const auto request = request_of(DOVI_TARGET_PROFILE_8_1);
	std::vector<uint8_t> output(2, 0xa5);
	uint64_t size = output.size();
	dovi_transform_info info{};
	EXPECT_EQ(
		dovi_transform_sample(&sample, &request, output.data(), &size, &info),
		DOVI_OUTPUT_TOO_SMALL);
	EXPECT_TRUE(size > output.size());
	EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xa5; }));
}

void test_av1_t35_supports_payload_complete_and_capacity_queries() {
	const auto input = rpu(8);
	std::vector<uint8_t> payload;
	std::vector<uint8_t> complete_output;
	for (uint32_t complete : {0u, 1u}) {
		uint64_t size = 0;
		EXPECT_EQ(
			dovi_write_av1_t35(
				input.data(), input.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
				complete, nullptr, &size),
			DOVI_OUTPUT_TOO_SMALL);
		std::vector<uint8_t> output(size);
		EXPECT_EQ(
			dovi_write_av1_t35(
				input.data(), input.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
				complete, output.data(), &size),
			DOVI_OK);
		if (complete == 0) payload = output;
		else complete_output = output;
	}
	EXPECT_TRUE(!payload.empty());
	EXPECT_TRUE(!complete_output.empty());
	EXPECT_EQ(payload.front(), 0x00);
	EXPECT_EQ(complete_output.front(), 0xB5);
	EXPECT_EQ(complete_output.size(), payload.size() + 1);
	EXPECT_TRUE(std::equal(payload.begin(), payload.end(), complete_output.begin() + 1));
}

void test_failures_never_publish_partial_output() {
	for (const auto& entry : std::vector<std::pair<uint8_t, dovi_status>>{
		{0xee, DOVI_RPU_PARSE_FAILED},
		{0xfd, DOVI_RPU_CONVERT_FAILED},
		{0xfc, DOVI_RPU_WRITE_FAILED},
	}) {
		const auto bytes = annex_b({rpu(entry.first)});
		const auto sample = sample_of(bytes, DOVI_FRAMING_ANNEX_B);
		std::vector<uint8_t> output(64, 0xa5);
		uint64_t size = output.size();
		const auto request = request_of(DOVI_TARGET_PROFILE_8_1);
		EXPECT_EQ(
			dovi_transform_sample(&sample, &request, output.data(), &size, nullptr),
			entry.second);
		EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xa5; }));
	}

	const auto repair_bytes = annex_b({rpu(0xfb)});
	const auto repair_sample = sample_of(repair_bytes, DOVI_FRAMING_ANNEX_B);
	const auto repair_request = request_of(
		DOVI_TARGET_LOSSLESS_REWRITE, DOVI_REPAIR_REMOVE_MAPPING);
	std::vector<uint8_t> repair_output(64, 0xa5);
	uint64_t repair_size = repair_output.size();
	EXPECT_EQ(
		dovi_transform_sample(
			&repair_sample, &repair_request, repair_output.data(), &repair_size, nullptr),
		DOVI_REPAIR_FAILED);
	EXPECT_TRUE(std::all_of(
		repair_output.begin(), repair_output.end(), [](uint8_t byte) { return byte == 0xa5; }));

	const auto exception_bytes = annex_b({rpu(0xfa)});
	const auto exception_sample = sample_of(exception_bytes, DOVI_FRAMING_ANNEX_B);
	const auto exception_request = request_of(DOVI_TARGET_PROFILE_8_1);
	std::vector<uint8_t> exception_output(64, 0xa5);
	uint64_t exception_size = exception_output.size();
	dovi_transform_info exception_info{};
	std::memset(&exception_info, 0x5a, sizeof(exception_info));
	EXPECT_EQ(
		dovi_transform_sample(
			&exception_sample, &exception_request, exception_output.data(),
			&exception_size, &exception_info),
		DOVI_INTERNAL_ERROR);
	EXPECT_EQ(exception_size, static_cast<uint64_t>(exception_output.size()));
	EXPECT_TRUE(std::all_of(
		exception_output.begin(), exception_output.end(), [](uint8_t byte) { return byte == 0xa5; }));
	const auto* info_bytes = reinterpret_cast<const uint8_t*>(&exception_info);
	EXPECT_TRUE(std::all_of(
		info_bytes, info_bytes + sizeof(exception_info), [](uint8_t byte) { return byte == 0x5a; }));

	dovi_sample_info inspection_info{};
	std::memset(&inspection_info, 0x6b, sizeof(inspection_info));
	EXPECT_EQ(dovi_inspect_sample(&exception_sample, &inspection_info), DOVI_INTERNAL_ERROR);
	const auto* inspection_bytes = reinterpret_cast<const uint8_t*>(&inspection_info);
	EXPECT_TRUE(std::all_of(
		inspection_bytes, inspection_bytes + sizeof(inspection_info),
		[](uint8_t byte) { return byte == 0x6b; }));

	const auto exception_rpu = rpu(0xfa);
	std::vector<uint8_t> av1_output(64, 0xa5);
	uint64_t av1_size = av1_output.size();
	EXPECT_EQ(
		dovi_write_av1_t35(
			exception_rpu.data(), exception_rpu.size(), DOVI_RPU_FORMAT_UNSPEC62_NAL,
			1, av1_output.data(), &av1_size),
		DOVI_INTERNAL_ERROR);
	EXPECT_EQ(av1_size, static_cast<uint64_t>(av1_output.size()));
	EXPECT_TRUE(std::all_of(
		av1_output.begin(), av1_output.end(), [](uint8_t byte) { return byte == 0xa5; }));
}

void test_mpv_request_and_error_state_is_typed_and_consumable() {
	uint64_t cleared_generation = 0;
	EXPECT_EQ(dovi_set_mpv_request_v3(nullptr, &cleared_generation), DOVI_OK);
	EXPECT_TRUE(cleared_generation != 0);
	dovi_transform_request stored{};
	uint64_t stored_generation = 0;
	EXPECT_EQ(dovi_get_mpv_request_v3(&stored, &stored_generation), 0u);
	EXPECT_EQ(stored_generation, cleared_generation);

	const auto request = request_of(
		DOVI_TARGET_PROFILE_8_1, DOVI_REPAIR_ZERO_ACTIVE_AREA);
	uint64_t first_generation = 0;
	EXPECT_EQ(dovi_set_mpv_request_v3(&request, &first_generation), DOVI_OK);
	EXPECT_TRUE(first_generation > cleared_generation);
	EXPECT_EQ(dovi_get_mpv_request_v3(&stored, &stored_generation), 1u);
	EXPECT_EQ(stored_generation, first_generation);
	EXPECT_EQ(stored.target, request.target);
	EXPECT_EQ(stored.repair_flags, request.repair_flags);
	dovi_transform_info observed{};
	EXPECT_EQ(dovi_get_mpv_transform_info_v3(first_generation, &observed), 0u);
	dovi_record_mpv_transform_v3(
		first_generation,
		DOVI_PRESENTATION_PROFILE_7_FEL,
		DOVI_PRESENTATION_PROFILE_8_1);
	EXPECT_EQ(dovi_get_mpv_transform_info_v3(first_generation, &observed), 1u);
	EXPECT_EQ(observed.input_presentation, DOVI_PRESENTATION_PROFILE_7_FEL);
	EXPECT_EQ(observed.output_presentation, DOVI_PRESENTATION_PROFILE_8_1);
	dovi_record_mpv_transform_v3(
		first_generation,
		DOVI_PRESENTATION_PROFILE_7_MEL,
		DOVI_PRESENTATION_PROFILE_8_4);
	EXPECT_EQ(dovi_get_mpv_transform_info_v3(first_generation, &observed), 1u);
	EXPECT_EQ(observed.input_presentation, DOVI_PRESENTATION_PROFILE_7_FEL);
	EXPECT_EQ(observed.output_presentation, DOVI_PRESENTATION_PROFILE_8_1);
	const auto invalid = request_of(
		DOVI_TARGET_LOSSLESS_REWRITE,
		DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS | DOVI_REPAIR_REMOVE_CMV40);
	uint64_t invalid_generation = 0;
	EXPECT_EQ(dovi_set_mpv_request_v3(&invalid, &invalid_generation), DOVI_INVALID_ARGUMENT);
	EXPECT_EQ(invalid_generation, 0u);
	EXPECT_EQ(dovi_get_mpv_request_v3(&stored, &stored_generation), 1u);
	EXPECT_EQ(stored_generation, first_generation);

	dovi_reset_mpv_error_v3(first_generation);
	dovi_record_mpv_error_v3(first_generation, DOVI_RPU_CONVERT_FAILED);
	dovi_record_mpv_error_v3(first_generation, DOVI_RPU_WRITE_FAILED);
	EXPECT_EQ(dovi_consume_mpv_error_v3(first_generation), DOVI_RPU_CONVERT_FAILED);
	EXPECT_EQ(dovi_consume_mpv_error_v3(first_generation), DOVI_OK);

	uint64_t second_generation = 0;
	EXPECT_EQ(dovi_set_mpv_request_v3(&request, &second_generation), DOVI_OK);
	EXPECT_TRUE(second_generation > first_generation);
	EXPECT_EQ(dovi_get_mpv_transform_info_v3(second_generation, &observed), 0u);
	dovi_record_mpv_transform_v3(
		first_generation,
		DOVI_PRESENTATION_PROFILE_7_MEL,
		DOVI_PRESENTATION_PROFILE_8_1);
	EXPECT_EQ(dovi_get_mpv_transform_info_v3(second_generation, &observed), 0u);
	dovi_reset_mpv_error_v3(second_generation);
	dovi_record_mpv_error_v3(first_generation, DOVI_INTERNAL_ERROR);
	EXPECT_EQ(dovi_consume_mpv_error_v3(second_generation), DOVI_OK);
	dovi_record_mpv_error_v3(second_generation, DOVI_RPU_WRITE_FAILED);
	dovi_record_mpv_error_v3(second_generation, DOVI_RPU_CONVERT_FAILED);
	EXPECT_EQ(dovi_consume_mpv_error_v3(second_generation), DOVI_RPU_WRITE_FAILED);
	EXPECT_EQ(dovi_consume_mpv_error_v3(first_generation), DOVI_OK);

	uint64_t final_clear_generation = 0;
	EXPECT_EQ(dovi_set_mpv_request_v3(nullptr, &final_clear_generation), DOVI_OK);
	EXPECT_TRUE(final_clear_generation > second_generation);
	dovi_record_mpv_error_v3(second_generation, DOVI_REPAIR_FAILED);
	EXPECT_EQ(dovi_consume_mpv_error_v3(final_clear_generation), DOVI_OK);

	EXPECT_EQ(dovi_set_mpv_request(&request), DOVI_OK);
	stored = request;
	EXPECT_EQ(dovi_get_mpv_request(&stored), 0u);
	EXPECT_EQ(stored.struct_size, 0u);
	dovi_record_mpv_error(DOVI_INTERNAL_ERROR);
	EXPECT_EQ(dovi_consume_mpv_error(), DOVI_OK);
}

} // namespace

extern "C" {

struct DoviRpuOpaque {};

DoviRpuOpaque* dovi_parse_unspec62_nalu(const uint8_t* data, size_t size) {
	parse_calls++;
	if (size >= 3 && data[2] == 0xfa) throw std::bad_alloc();
	auto* fake = new FakeRpu;
	fake->bytes.assign(data, data + size);
	if (size < 4 || data[0] != 0x7c || data[1] != 0x01 || data[2] == 0xee) {
		fake->error = "parse failed";
	} else {
		fake->failure_code = data[2];
		fake->profile = data[2] >= 0xfb ? 7 : data[2];
		fake->el_type = data[3] == 1 ? "MEL" : "FEL";
		fake->fail_write = data[2] == 0xfc;
	}
	return reinterpret_cast<DoviRpuOpaque*>(fake);
}

DoviRpuOpaque* dovi_parse_rpu(const uint8_t* data, size_t size) {
	std::vector<uint8_t> nalu{0x7c, 0x01};
	nalu.insert(nalu.end(), data, data + size);
	return dovi_parse_unspec62_nalu(nalu.data(), nalu.size());
}

const char* dovi_rpu_get_error(const DoviRpuOpaque* rpu) {
	const auto* fake = reinterpret_cast<const FakeRpu*>(rpu);
	return fake->error.empty() ? nullptr : fake->error.c_str();
}

int32_t dovi_rpu_get_profile(const DoviRpuOpaque* rpu) {
	const auto* fake = reinterpret_cast<const FakeRpu*>(rpu);
	return fake->profile;
}

int32_t dovi_rpu_get_el_type(const DoviRpuOpaque* rpu) {
	const auto* fake = reinterpret_cast<const FakeRpu*>(rpu);
	if (fake->profile != 7) return 0;
	return fake->el_type == "MEL" ? 1 : 2;
}

int32_t dovi_rpu_has_mapping(const DoviRpuOpaque* rpu) {
	return reinterpret_cast<const FakeRpu*>(rpu)->profile != 5 ? 1 : 0;
}

int32_t dovi_rpu_has_cmv40_metadata(const DoviRpuOpaque* rpu) {
	return reinterpret_cast<const FakeRpu*>(rpu)->el_type == "FEL" ? 1 : 0;
}

int32_t dovi_convert_rpu_with_mode(DoviRpuOpaque* rpu, uint8_t mode) {
	auto* fake = reinterpret_cast<FakeRpu*>(rpu);
	if (fake->failure_code == 0xfd) return -1;
	fake->conversion = mode;
	observed_conversion_modes.push_back(mode);
	if (mode == 1) {
		fake->profile = 7;
		fake->el_type = "MEL";
	} else if (mode == 2 || mode == 4) {
		fake->profile = 8;
		fake->el_type.clear();
	}
	fake->bytes[2] = fake->profile;
	fake->bytes[3] = fake->el_type == "MEL" ? 1 : 2;
	return 0;
}

int32_t dovi_convert_rpu_to_p81_preserve_mapping(DoviRpuOpaque* rpu) {
	auto* fake = reinterpret_cast<FakeRpu*>(rpu);
	if (fake->failure_code == 0xfd) return -1;
	preserve_mapping_calls++;
	fake->profile = 8;
	fake->el_type.clear();
	fake->bytes[2] = 8;
	fake->bytes[3] = 3;
	return 0;
}

int32_t dovi_rpu_remove_mapping(DoviRpuOpaque* rpu) {
	auto* fake = reinterpret_cast<FakeRpu*>(rpu);
	if (fake->failure_code == 0xfb) return -1;
	fake->repairs |= DOVI_REPAIR_REMOVE_MAPPING;
	return 0;
}

int32_t dovi_rpu_set_active_area_offsets(DoviRpuOpaque* rpu, uint16_t, uint16_t, uint16_t, uint16_t) {
	reinterpret_cast<FakeRpu*>(rpu)->repairs |= DOVI_REPAIR_ZERO_ACTIVE_AREA;
	return 0;
}

int32_t dovi_rpu_add_cmv40_safe_default_metadata(DoviRpuOpaque* rpu) {
	reinterpret_cast<FakeRpu*>(rpu)->repairs |= DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS;
	return 1;
}

int32_t dovi_rpu_remove_cmv40_metadata(DoviRpuOpaque* rpu) {
	reinterpret_cast<FakeRpu*>(rpu)->repairs |= DOVI_REPAIR_REMOVE_CMV40;
	return 0;
}

const DoviData* write_data(const std::vector<uint8_t>& bytes) {
	auto* owned = static_cast<uint8_t*>(std::malloc(bytes.size()));
	std::memcpy(owned, bytes.data(), bytes.size());
	return new DoviData{owned, bytes.size()};
}

const DoviData* dovi_write_unspec62_nalu(DoviRpuOpaque* rpu) {
	const auto* fake = reinterpret_cast<const FakeRpu*>(rpu);
	if (fake->fail_write) return nullptr;
	auto bytes = fake->bytes;
	if (fake->repairs != 0) bytes.push_back(static_cast<uint8_t>(fake->repairs));
	return write_data(bytes);
}

const DoviData* dovi_write_av1_rpu_metadata_obu_t35_payload(DoviRpuOpaque*) {
	return write_data({0x00, 0x3b, 0x01});
}

const DoviData* dovi_write_av1_rpu_metadata_obu_t35_complete(DoviRpuOpaque*) {
	return write_data({0xB5, 0x00, 0x3b, 0x01});
}

void dovi_data_free(const DoviData* data) {
	std::free(const_cast<uint8_t*>(data->data));
	delete data;
}

void dovi_rpu_free(DoviRpuOpaque* rpu) {
	delete reinterpret_cast<FakeRpu*>(rpu);
}

} // extern "C"

int main() {
	test_abi_and_capabilities_are_semantic();
	test_inspection_supports_all_framings_and_supplemental_rpu();
	test_inspection_rejects_malformed_and_inconsistent_rpus();
	test_lossless_rewrite_preserves_non_rpu_units();
	test_owned_transform_converts_once_and_releases_output();
	test_mel_and_profile81_targets_cover_supported_profiles();
	test_profile84_requires_an_hlg_encoded_base();
	test_source_base_drops_only_dolby_vision_data_and_preserves_hdr10_plus();
	test_preserved_nals_do_not_add_heap_allocations_per_nal();
	test_repairs_apply_independently_and_reject_conflicts();
	test_supplemental_rpu_is_inserted_before_the_first_vcl();
	test_capacity_negotiation_is_atomic();
	test_av1_t35_supports_payload_complete_and_capacity_queries();
	test_failures_never_publish_partial_output();
	test_mpv_request_and_error_state_is_typed_and_consumable();

	if (failures == 0) std::cout << "All dovi native tests passed\n";
	return failures == 0 ? 0 : 1;
}

#endif
