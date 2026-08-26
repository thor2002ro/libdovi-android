#include "dovi.h"

#include <limits>
#include <mutex>

namespace {

constexpr uint32_t kKnownRepairFlags =
	DOVI_REPAIR_REMOVE_MAPPING |
	DOVI_REPAIR_ZERO_ACTIVE_AREA |
	DOVI_REPAIR_ADD_CMV40_SAFE_DEFAULTS |
	DOVI_REPAIR_REMOVE_CMV40;

struct MpvState {
	uint64_t generation = 0;
	bool active = false;
	dovi_transform_request request{};
	dovi_status error = DOVI_OK;
};

std::mutex mpv_mutex;
MpvState mpv_state;

dovi_status validate_request(const dovi_transform_request* request) {
	if (request->struct_size != sizeof(dovi_transform_request) ||
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

} // namespace

extern "C" {

dovi_status dovi_set_mpv_request(const dovi_transform_request*) {
	return DOVI_OK;
}

uint32_t dovi_get_mpv_request(dovi_transform_request* request) {
	if (request != nullptr) *request = {};
	return 0;
}

void dovi_reset_mpv_error(void) {}

void dovi_record_mpv_error(dovi_status) {}

dovi_status dovi_consume_mpv_error(void) {
	return DOVI_OK;
}

dovi_status dovi_set_mpv_request_v3(
	const dovi_transform_request* request,
	uint64_t* generation
) {
	if (generation == nullptr) return DOVI_INVALID_ARGUMENT;
	if (request != nullptr) {
		const auto status = validate_request(request);
		if (status != DOVI_OK) return status;
	}

	std::lock_guard<std::mutex> lock(mpv_mutex);
	if (mpv_state.generation == static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		return DOVI_INTERNAL_ERROR;
	}
	mpv_state.generation++;
	mpv_state.active = request != nullptr;
	mpv_state.request = request != nullptr ? *request : dovi_transform_request{};
	mpv_state.error = DOVI_OK;
	*generation = mpv_state.generation;
	return DOVI_OK;
}

uint32_t dovi_get_mpv_request_v3(
	dovi_transform_request* request,
	uint64_t* generation
) {
	if (request == nullptr || generation == nullptr) return 0;
	std::lock_guard<std::mutex> lock(mpv_mutex);
	*generation = mpv_state.generation;
	if (!mpv_state.active) {
		*request = {};
		return 0;
	}
	*request = mpv_state.request;
	return 1;
}

void dovi_reset_mpv_error_v3(uint64_t generation) {
	std::lock_guard<std::mutex> lock(mpv_mutex);
	if (generation == mpv_state.generation) mpv_state.error = DOVI_OK;
}

void dovi_record_mpv_error_v3(uint64_t generation, dovi_status status) {
	if (status == DOVI_OK) return;
	std::lock_guard<std::mutex> lock(mpv_mutex);
	if (generation == mpv_state.generation && mpv_state.error == DOVI_OK) {
		mpv_state.error = status;
	}
}

dovi_status dovi_consume_mpv_error_v3(uint64_t generation) {
	std::lock_guard<std::mutex> lock(mpv_mutex);
	if (generation != mpv_state.generation) return DOVI_OK;
	const auto status = mpv_state.error;
	mpv_state.error = DOVI_OK;
	return status;
}

} // extern "C"
