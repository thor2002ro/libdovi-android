package io.github.thor2002ro.libdovi

/** The HEVC sample framing supplied to libdovi. */
enum class DoviFraming {
	AUTO,
	ANNEX_B,
	LENGTH_PREFIXED,
}

/** The encoded-pixel presentation used as independent base-layer evidence. */
enum class DoviPresentation {
	UNKNOWN,
	PROFILE_5,
	PROFILE_7_MEL,
	PROFILE_7_FEL,
	PROFILE_8_1,
	PROFILE_8_4,
	HDR10,
	HDR10_PLUS,
	HLG,
}

/** A semantic transform target. Values do not expose upstream conversion modes. */
enum class DoviTarget {
	LOSSLESS_REWRITE,
	MEL,
	PROFILE_8_1,
	PROFILE_8_1_PRESERVE_MAPPING,
	PROFILE_8_4,
	SOURCE_BASE_PRESENTATION,
}

/** An optional metadata repair applied after conversion. */
enum class DoviRepair {
	REMOVE_MAPPING,
	ZERO_ACTIVE_AREA,
	ADD_CMV40_SAFE_DEFAULTS,
	REMOVE_CMV40,
}

/** A capability advertised by the loaded ABI. */
enum class DoviCapability {
	INSPECT,
	VALIDATE,
	LOSSLESS_REWRITE,
	MEL,
	PROFILE_8_1,
	PROFILE_8_1_PRESERVE_MAPPING,
	PROFILE_8_4,
	SOURCE_BASE_PRESENTATION,
	REPAIR_REMOVE_MAPPING,
	REPAIR_ZERO_ACTIVE_AREA,
	REPAIR_ADD_CMV40_SAFE_DEFAULTS,
	REPAIR_REMOVE_CMV40,
	AV1_T35,
	MPV_STATE,
	MPV_TRANSFORM_OBSERVATION,
}

/** A fixed-width status defined by the stable C ABI. */
enum class DoviStatus {
	OK,
	INVALID_ARGUMENT,
	MALFORMED_SAMPLE,
	OUTPUT_TOO_SMALL,
	RPU_PARSE_FAILED,
	RPU_CONVERT_FAILED,
	RPU_WRITE_FAILED,
	UNSUPPORTED_FRAMING,
	INTERNAL_ERROR,
	RPU_NOT_FOUND,
	UNSUPPORTED_PROFILE,
	REENCODE_REQUIRED,
	REPAIR_FAILED,
	INCONSISTENT_RPU,
}

/** The form of RPU data supplied to [DoviBridge.writeAv1T35]. */
enum class DoviRpuFormat {
	RAW,
	UNSPEC62_NAL,
}

/** A complete HEVC access-unit sample and the base-pixel facts needed for safe conversion. */
data class DoviSample(
	val bytes: ByteArray,
	val framing: DoviFraming = DoviFraming.AUTO,
	val nalLengthSize: Int = 0,
	val sourceBasePresentation: DoviPresentation = DoviPresentation.UNKNOWN,
	val supplementalRpu: ByteArray? = null,
	val bytesOffset: Int = 0,
	val bytesSize: Int = bytes.size - bytesOffset,
	val supplementalRpuOffset: Int = 0,
	val supplementalRpuSize: Int = supplementalRpu?.size?.minus(supplementalRpuOffset) ?: 0,
) {
	init {
		require(bytesOffset >= 0 && bytesSize > 0 && bytesOffset <= bytes.size - bytesSize) {
			"Sample byte range must be non-empty and within its backing array"
		}
		require(
			supplementalRpuOffset >= 0 && supplementalRpuSize >= 0 &&
				if (supplementalRpu == null) {
					supplementalRpuOffset == 0 && supplementalRpuSize == 0
				} else {
					supplementalRpuOffset <= supplementalRpu.size - supplementalRpuSize
				},
		) { "Supplemental RPU byte range must be within its backing array" }
		require(
			sourceBasePresentation == DoviPresentation.UNKNOWN ||
				sourceBasePresentation == DoviPresentation.HDR10 ||
				sourceBasePresentation == DoviPresentation.HDR10_PLUS ||
				sourceBasePresentation == DoviPresentation.HLG,
		) {
			"Source base presentation must describe encoded base pixels"
		}
		when (framing) {
			DoviFraming.LENGTH_PREFIXED -> require(nalLengthSize == 1 || nalLengthSize == 2 || nalLengthSize == 4) {
				"Length-prefixed samples require a 1, 2, or 4 byte NAL length"
			}
			DoviFraming.AUTO,
			DoviFraming.ANNEX_B,
			-> require(nalLengthSize == 0) {
				"NAL length size is only valid for length-prefixed samples"
			}
		}
	}
}

/** A semantic request validated before it crosses JNI. */
class DoviTransformRequest(
	val target: DoviTarget,
	repairs: Set<DoviRepair> = emptySet(),
) {
	val repairs: Set<DoviRepair> = java.util.Collections.unmodifiableSet(repairs.toSet())

	init {
		require(
			DoviRepair.ADD_CMV40_SAFE_DEFAULTS !in this.repairs ||
				DoviRepair.REMOVE_CMV40 !in this.repairs,
		) { "CM v4.0 metadata cannot be added and removed in one request" }
		require(target != DoviTarget.SOURCE_BASE_PRESENTATION || this.repairs.isEmpty()) {
			"Source-base output cannot apply RPU repairs"
		}
	}

	override fun equals(other: Any?): Boolean =
		this === other || other is DoviTransformRequest &&
			target == other.target && repairs == other.repairs

	override fun hashCode(): Int = 31 * target.hashCode() + repairs.hashCode()

	override fun toString(): String = "DoviTransformRequest(target=$target, repairs=$repairs)"

	fun copy(
		target: DoviTarget = this.target,
		repairs: Set<DoviRepair> = this.repairs,
	): DoviTransformRequest = DoviTransformRequest(target, repairs)

	operator fun component1(): DoviTarget = target

	operator fun component2(): Set<DoviRepair> = repairs
}

/** Opaque ownership token for one published MPV native request. */
interface DoviMpvSession

internal class NativeDoviMpvSession(
	internal val generation: Long,
) : DoviMpvSession {
	init {
		require(generation > 0) { "MPV session generation must be nonzero" }
	}
}

/** Inspection facts returned by the stable ABI. */
data class DoviInspection(
	val input: DoviPresentation,
	val rpuCount: Int,
	val enhancementNalCount: Int,
	val videoNalCount: Int,
	val framing: DoviFraming,
	val nalLengthSize: Int,
	val mappingPresent: Boolean = false,
	val cmv40Present: Boolean = false,
)

data class DoviTransformResult(
	val bytes: ByteArray,
	val output: DoviPresentation,
	val appliedRepairs: Set<DoviRepair>,
	val input: DoviPresentation,
)

/** Caller-owned output storage for repeated transforms on one playback thread. */
class DoviTransformBuffer {
	internal var bytes = ByteArray(0)
	internal val outputSize = LongArray(1)
	internal val info = IntArray(6)

	internal fun ensureCapacity(required: Int) {
		if (required <= bytes.size) return
		val capacity = (required.toLong() + required / 2).coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
		bytes = bytes.copyOf(capacity)
	}
}

/** A view into [DoviTransformBuffer] that remains valid until that buffer is used again. */
data class DoviTransformBufferResult(
	val bytes: ByteArray,
	val bytesSize: Int,
	val output: DoviPresentation,
	val appliedRepairs: Set<DoviRepair>,
	val input: DoviPresentation,
)

/** The first successful native transform observed for an active MPV request. */
data class DoviTransformObservation(
	val input: DoviPresentation,
	val output: DoviPresentation,
)

/** A typed native failure. */
class DoviException internal constructor(
	val status: DoviStatus,
	val operation: String,
	detail: String? = null,
) : RuntimeException(
	buildString {
		append(operation)
		append(" failed with ")
		append(status.name)
		if (detail != null) {
			append(": ")
			append(detail)
		}
	},
)

/** The JNI library could not be loaded or did not expose the required ABI. */
class DoviUnavailableException internal constructor(
	val operation: String,
	cause: Throwable,
) : IllegalStateException("$operation requires libdovi ABI version 3", cause)

internal const val DOVI_KOTLIN_ABI_VERSION = 3

internal fun verifyDoviAbiVersion(actualVersion: Int) {
	check(actualVersion == DOVI_KOTLIN_ABI_VERSION) {
		"Unsupported libdovi ABI version $actualVersion; expected $DOVI_KOTLIN_ABI_VERSION"
	}
}
