#include "dovi.h"

#if defined(__ANDROID__)

#include <jni.h>

#include <array>
#include <cstdint>
#include <limits>

namespace {

class ByteArrayAccess {
public:
	ByteArrayAccess(JNIEnv* env, jbyteArray array, bool writable = false)
		: env_(env), array_(array), writable_(writable) {
		if (array_ != nullptr) bytes_ = env_->GetByteArrayElements(array_, nullptr);
	}

	~ByteArrayAccess() {
		if (bytes_ != nullptr) {
			env_->ReleaseByteArrayElements(array_, bytes_, writable_ ? 0 : JNI_ABORT);
		}
	}

	uint8_t* data() const { return reinterpret_cast<uint8_t*>(bytes_); }
	uint64_t size() const {
		return array_ == nullptr ? 0 : static_cast<uint64_t>(env_->GetArrayLength(array_));
	}
	bool valid() const { return array_ == nullptr || bytes_ != nullptr; }

private:
	JNIEnv* env_;
	jbyteArray array_;
	bool writable_;
	jbyte* bytes_ = nullptr;
};

class OwnedDoviBuffer {
public:
	~OwnedDoviBuffer() { dovi_owned_buffer_free(&value); }

	dovi_owned_buffer value{};
};

bool read_output_size(JNIEnv* env, jlongArray sizes, uint64_t* size) {
	jlong value = 0;
	env->GetLongArrayRegion(sizes, 0, 1, &value);
	if (env->ExceptionCheck() || value < 0) return false;
	*size = static_cast<uint64_t>(value);
	return true;
}

bool valid_long_destination(JNIEnv* env, jlongArray destination, jsize minimum_size) {
	return destination != nullptr && env->GetArrayLength(destination) >= minimum_size &&
		!env->ExceptionCheck();
}

bool valid_int_destination(JNIEnv* env, jintArray destination, jsize minimum_size) {
	return destination != nullptr && env->GetArrayLength(destination) >= minimum_size &&
		!env->ExceptionCheck();
}

bool write_status(JNIEnv* env, jintArray destination, dovi_status status) {
	const auto value = static_cast<jint>(status);
	env->SetIntArrayRegion(destination, 0, 1, &value);
	return !env->ExceptionCheck();
}

void write_output_size(JNIEnv* env, jlongArray sizes, uint64_t size) {
	const auto value = size > static_cast<uint64_t>(std::numeric_limits<jlong>::max())
		? std::numeric_limits<jlong>::max()
		: static_cast<jlong>(size);
	env->SetLongArrayRegion(sizes, 0, 1, &value);
}

bool write_inspection(JNIEnv* env, jintArray destination, const dovi_sample_info& info) {
	const std::array<jint, 7> values{
		static_cast<jint>(info.input_presentation),
		static_cast<jint>(info.rpu_count),
		static_cast<jint>(info.enhancement_nal_count),
		static_cast<jint>(info.video_nal_count),
		static_cast<jint>(info.framing),
		static_cast<jint>(info.nal_length_size),
		static_cast<jint>(info.metadata_flags),
	};
	env->SetIntArrayRegion(destination, 0, values.size(), values.data());
	return !env->ExceptionCheck();
}

bool write_transform_info(JNIEnv* env, jintArray destination, const dovi_transform_info& info) {
	const std::array<jint, 6> values{
		static_cast<jint>(info.output_presentation),
		static_cast<jint>(info.applied_repair_flags),
		static_cast<jint>(info.converted_rpu_count),
		static_cast<jint>(info.dropped_dovi_nal_count),
		static_cast<jint>(info.preserved_nal_count),
		static_cast<jint>(info.input_presentation),
	};
	env->SetIntArrayRegion(destination, 0, values.size(), values.data());
	return !env->ExceptionCheck();
}

dovi_sample make_sample(
	const ByteArrayAccess& input,
	jint input_offset,
	jint input_size,
	jint framing,
	jint nal_length_size,
	jint source_base_presentation,
	const ByteArrayAccess& supplemental,
	jint supplemental_offset,
	jint supplemental_size
) {
	return {
		sizeof(dovi_sample),
		static_cast<uint32_t>(framing),
		static_cast<uint32_t>(nal_length_size),
		static_cast<uint32_t>(source_base_presentation),
		input.data() + input_offset,
		static_cast<uint64_t>(input_size),
		supplemental.data() == nullptr ? nullptr : supplemental.data() + supplemental_offset,
		static_cast<uint64_t>(supplemental_size),
	};
}

bool valid_range(const ByteArrayAccess& array, jint offset, jint size, bool require_non_empty) {
	if (offset < 0 || size < 0 || (require_non_empty && size == 0)) return false;
	const auto start = static_cast<uint64_t>(offset);
	const auto length = static_cast<uint64_t>(size);
	return start <= array.size() && length <= array.size() - start;
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeAbiVersion(JNIEnv*, jobject) {
	return static_cast<jint>(dovi_abi_version());
}

JNIEXPORT jlong JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeCapabilities(JNIEnv*, jobject) {
	return static_cast<jlong>(dovi_capabilities());
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeInspectSample(
	JNIEnv* env,
	jobject,
	jbyteArray input_array,
	jint input_offset,
	jint input_size,
	jint framing,
	jint nal_length_size,
	jint source_base_presentation,
	jbyteArray supplemental_array,
	jint supplemental_offset,
	jint supplemental_size,
	jintArray info_array
) {
	try {
		if (input_array == nullptr || !valid_int_destination(env, info_array, 7)) {
			return DOVI_INVALID_ARGUMENT;
		}
		ByteArrayAccess input(env, input_array);
		ByteArrayAccess supplemental(env, supplemental_array);
		if (!input.valid() || !supplemental.valid()) return DOVI_INTERNAL_ERROR;
		if (!valid_range(input, input_offset, input_size, true) ||
			!valid_range(supplemental, supplemental_offset, supplemental_size, false)) {
			return DOVI_INVALID_ARGUMENT;
		}
		const auto sample = make_sample(
			input, input_offset, input_size, framing, nal_length_size, source_base_presentation,
			supplemental, supplemental_offset, supplemental_size);
		dovi_sample_info info{};
		const auto status = dovi_inspect_sample(&sample, &info);
		if (status == DOVI_OK && !write_inspection(env, info_array, info)) {
			return DOVI_INTERNAL_ERROR;
		}
		return status;
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSample(
	JNIEnv* env,
	jobject,
	jbyteArray input_array,
	jint input_offset,
	jint input_size,
	jint framing,
	jint nal_length_size,
	jint source_base_presentation,
	jbyteArray supplemental_array,
	jint supplemental_offset,
	jint supplemental_size,
	jint target,
	jint repair_flags,
	jbyteArray output_array,
	jlongArray output_size_array,
	jintArray info_array
) {
	try {
		if (input_array == nullptr ||
			!valid_long_destination(env, output_size_array, 1) ||
			!valid_int_destination(env, info_array, 6)) {
			return DOVI_INVALID_ARGUMENT;
		}
		ByteArrayAccess input(env, input_array);
		ByteArrayAccess supplemental(env, supplemental_array);
		ByteArrayAccess output(env, output_array, true);
		if (!input.valid() || !supplemental.valid() || !output.valid()) return DOVI_INTERNAL_ERROR;
		if (!valid_range(input, input_offset, input_size, true) ||
			!valid_range(supplemental, supplemental_offset, supplemental_size, false)) {
			return DOVI_INVALID_ARGUMENT;
		}
		uint64_t output_size = 0;
		if (!read_output_size(env, output_size_array, &output_size)) return DOVI_INVALID_ARGUMENT;
		if (output_array != nullptr && output_size > output.size()) return DOVI_INVALID_ARGUMENT;
		const auto sample = make_sample(
			input, input_offset, input_size, framing, nal_length_size, source_base_presentation,
			supplemental, supplemental_offset, supplemental_size);
		const dovi_transform_request request{
			sizeof(dovi_transform_request),
			static_cast<uint32_t>(target),
			static_cast<uint32_t>(repair_flags),
			0,
		};
		dovi_transform_info info{};
		const auto status = dovi_transform_sample(
			&sample, &request, output.data(), &output_size, &info);
		if ((status == DOVI_OK || status == DOVI_OUTPUT_TOO_SMALL) &&
			!write_transform_info(env, info_array, info)) {
			return DOVI_INTERNAL_ERROR;
		}
		write_output_size(env, output_size_array, output_size);
		return status;
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

JNIEXPORT jbyteArray JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeTransformSampleAllocated(
	JNIEnv* env,
	jobject,
	jbyteArray input_array,
	jint input_offset,
	jint input_size,
	jint framing,
	jint nal_length_size,
	jint source_base_presentation,
	jbyteArray supplemental_array,
	jint supplemental_offset,
	jint supplemental_size,
	jint target,
	jint repair_flags,
	jintArray status_array,
	jintArray info_array
) {
	try {
		if (input_array == nullptr ||
			!valid_int_destination(env, status_array, 1) ||
			!valid_int_destination(env, info_array, 6)) {
			return nullptr;
		}
		ByteArrayAccess input(env, input_array);
		ByteArrayAccess supplemental(env, supplemental_array);
		if (!input.valid() || !supplemental.valid()) {
			write_status(env, status_array, DOVI_INTERNAL_ERROR);
			return nullptr;
		}
		if (!valid_range(input, input_offset, input_size, true) ||
			!valid_range(supplemental, supplemental_offset, supplemental_size, false)) {
			write_status(env, status_array, DOVI_INVALID_ARGUMENT);
			return nullptr;
		}
		const auto sample = make_sample(
			input, input_offset, input_size, framing, nal_length_size, source_base_presentation,
			supplemental, supplemental_offset, supplemental_size);
		const dovi_transform_request request{
			sizeof(dovi_transform_request),
			static_cast<uint32_t>(target),
			static_cast<uint32_t>(repair_flags),
			0,
		};
		dovi_transform_info info{};
		OwnedDoviBuffer output;
		auto status = dovi_transform_sample_alloc(
			&sample, &request, &output.value, &info);
		if (status != DOVI_OK) {
			write_status(env, status_array, status);
			return nullptr;
		}
		if (output.value.size == 0 ||
			output.value.size > static_cast<uint64_t>(std::numeric_limits<jsize>::max())) {
			write_status(env, status_array, DOVI_INTERNAL_ERROR);
			return nullptr;
		}
		auto bytes = env->NewByteArray(static_cast<jsize>(output.value.size));
		if (bytes == nullptr || env->ExceptionCheck()) return nullptr;
		env->SetByteArrayRegion(
			bytes,
			0,
			static_cast<jsize>(output.value.size),
			reinterpret_cast<const jbyte*>(output.value.data));
		if (env->ExceptionCheck() || !write_transform_info(env, info_array, info) ||
			!write_status(env, status_array, DOVI_OK)) {
			return nullptr;
		}
		return bytes;
	} catch (...) {
		if (status_array != nullptr && !env->ExceptionCheck()) {
			write_status(env, status_array, DOVI_INTERNAL_ERROR);
		}
		return nullptr;
	}
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeWriteAv1T35(
	JNIEnv* env,
	jobject,
	jbyteArray rpu_array,
	jint rpu_format,
	jboolean complete_obu,
	jbyteArray output_array,
	jlongArray output_size_array
) {
	try {
		if (rpu_array == nullptr || !valid_long_destination(env, output_size_array, 1)) {
			return DOVI_INVALID_ARGUMENT;
		}
		ByteArrayAccess rpu(env, rpu_array);
		ByteArrayAccess output(env, output_array, true);
		if (!rpu.valid() || !output.valid()) return DOVI_INTERNAL_ERROR;
		uint64_t output_size = 0;
		if (!read_output_size(env, output_size_array, &output_size)) return DOVI_INVALID_ARGUMENT;
		if (output_array != nullptr && output_size > output.size()) return DOVI_INVALID_ARGUMENT;
		const auto status = dovi_write_av1_t35(
			rpu.data(),
			rpu.size(),
			static_cast<uint32_t>(rpu_format),
			complete_obu == JNI_TRUE ? 1u : 0u,
			output.data(),
			&output_size);
		write_output_size(env, output_size_array, output_size);
		return status;
	} catch (...) {
		return DOVI_INTERNAL_ERROR;
	}
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeSetMpvRequest(
	JNIEnv* env,
	jobject,
	jint target,
	jint repair_flags,
	jlongArray generation_array
) {
	if (!valid_long_destination(env, generation_array, 1)) return DOVI_INVALID_ARGUMENT;
	const dovi_transform_request request{
		sizeof(dovi_transform_request),
		static_cast<uint32_t>(target),
		static_cast<uint32_t>(repair_flags),
		0,
	};
	uint64_t generation = 0;
	const auto status = dovi_set_mpv_request_v3(target < 0 ? nullptr : &request, &generation);
	if (status == DOVI_OK) write_output_size(env, generation_array, generation);
	return status;
}

JNIEXPORT void JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeResetMpvError(
	JNIEnv*, jobject, jlong generation
) {
	if (generation > 0) dovi_reset_mpv_error_v3(static_cast<uint64_t>(generation));
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeConsumeMpvError(
	JNIEnv*, jobject, jlong generation
) {
	if (generation <= 0) return DOVI_INVALID_ARGUMENT;
	return static_cast<jint>(dovi_consume_mpv_error_v3(static_cast<uint64_t>(generation)));
}

JNIEXPORT jint JNICALL
Java_io_github_thor2002ro_libdovi_DoviBridge_nativeGetMpvTransformObservation(
	JNIEnv* env,
	jobject,
	jlong generation,
	jintArray info_array
) {
	if (generation <= 0 || !valid_int_destination(env, info_array, 2)) return DOVI_INVALID_ARGUMENT;
	dovi_transform_info info{};
	if (dovi_get_mpv_transform_info_v3(static_cast<uint64_t>(generation), &info) == 0) return 0;
	const std::array<jint, 2> values{
		static_cast<jint>(info.input_presentation),
		static_cast<jint>(info.output_presentation),
	};
	env->SetIntArrayRegion(info_array, 0, values.size(), values.data());
	return env->ExceptionCheck() ? DOVI_INTERNAL_ERROR : 1;
}

} // extern "C"

#endif
