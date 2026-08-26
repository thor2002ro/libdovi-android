package io.github.thor2002ro.libdovi

/** Reusable Kotlin entry point for the stable libdovi Android ABI. */
object DoviBridge {
	private const val LIBRARY_NAME = "jellyfin_dovi"

	private val api = DoviApi(JniBackend)

	/** Returns whether the JNI library loaded and exposes the supported ABI version. */
	fun isAvailable(): Boolean = api.isAvailable()

	fun inspect(sample: DoviSample): DoviInspection = api.inspect(sample)

	fun inspect(
		bytes: ByteArray,
		framing: DoviFraming = DoviFraming.AUTO,
		nalLengthSize: Int = 0,
		sourceBasePresentation: DoviPresentation = DoviPresentation.UNKNOWN,
		supplementalRpu: ByteArray? = null,
	): DoviInspection = inspect(
		DoviSample(bytes, framing, nalLengthSize, sourceBasePresentation, supplementalRpu),
	)

	fun transform(sample: DoviSample, request: DoviTransformRequest): DoviTransformResult =
		api.transform(sample, request)

	fun transform(
		sample: DoviSample,
		request: DoviTransformRequest,
		buffer: DoviTransformBuffer,
	): DoviTransformBufferResult = api.transform(sample, request, buffer)

	fun openTransformSession(
		request: DoviTransformRequest,
		strategy: DoviTransformStrategy = DoviTransformStrategy.LIBDOVI,
		state: DoviTransformSessionState = DoviTransformSessionState(),
	): DoviTransformSession = api.openTransformSession(request, strategy, state)

	fun transform(
		bytes: ByteArray,
		request: DoviTransformRequest,
		framing: DoviFraming = DoviFraming.AUTO,
		nalLengthSize: Int = 0,
		sourceBasePresentation: DoviPresentation = DoviPresentation.UNKNOWN,
		supplementalRpu: ByteArray? = null,
	): DoviTransformResult = transform(
		DoviSample(bytes, framing, nalLengthSize, sourceBasePresentation, supplementalRpu),
		request,
	)

	fun writeAv1T35(
		rpu: ByteArray,
		format: DoviRpuFormat = DoviRpuFormat.RAW,
		completeObu: Boolean = false,
	): ByteArray = api.writeAv1T35(rpu, format, completeObu)

	fun capabilities(): Set<DoviCapability> = api.capabilities()

	fun setMpvRequest(request: DoviTransformRequest?): DoviMpvSession = api.setMpvRequest(request)

	fun resetMpvError(session: DoviMpvSession) = api.resetMpvError(session)

	fun consumeMpvError(session: DoviMpvSession): DoviStatus = api.consumeMpvError(session)

	fun getMpvTransformObservation(session: DoviMpvSession): DoviTransformObservation? =
		api.getMpvTransformObservation(session)

		private object JniBackend : DoviBackend {
		override fun initialize() {
			System.loadLibrary(LIBRARY_NAME)
			verifyDoviAbiVersion(nativeAbiVersion())
		}

		override fun inspect(sample: DoviSample): DoviInspection {
			val info = IntArray(INSPECTION_FIELD_COUNT)
			checkStatus(
				operation = "inspect",
				value = nativeInspectSample(
					sample.bytes,
					sample.bytesOffset,
					sample.bytesSize,
					framingToNative(sample.framing),
					sample.nalLengthSize,
					presentationToNative(sample.sourceBasePresentation),
					sample.supplementalRpu,
					sample.supplementalRpuOffset,
					sample.supplementalRpuSize,
					info,
				),
			)
			return DoviInspection(
				input = presentationFromNative(info[0], "inspect"),
				rpuCount = nonNegative(info[1], "RPU count", "inspect"),
				enhancementNalCount = nonNegative(info[2], "enhancement NAL count", "inspect"),
				videoNalCount = nonNegative(info[3], "video NAL count", "inspect"),
				framing = framingFromNative(info[4], "inspect"),
				nalLengthSize = validReturnedNalLengthSize(info[5], "inspect"),
				mappingPresent = inspectionFlag(info[6], 1 shl 0),
				cmv40Present = inspectionFlag(info[6], 1 shl 1),
			)
		}

		override fun transform(
			sample: DoviSample,
			request: DoviTransformRequest,
		): DoviTransformResult {
			val info = IntArray(TRANSFORM_FIELD_COUNT)
			val status = IntArray(1)
			val bytes = nativeTransformSampleAllocated(
				sample.bytes,
				sample.bytesOffset,
				sample.bytesSize,
				framingToNative(sample.framing),
				sample.nalLengthSize,
				presentationToNative(sample.sourceBasePresentation),
				sample.supplementalRpu,
				sample.supplementalRpuOffset,
				sample.supplementalRpuSize,
				targetToNative(request.target),
				repairsToNative(request.repairs),
				status,
				info,
			)
			checkStatus("transform", status[0])
			return exactTransformResult(
				bytes = bytes ?: throw invalidNativeResult("transform", "native output was null"),
				input = presentationFromNative(info[5], "transform"),
				output = presentationFromNative(info[0], "transform"),
				appliedRepairs = repairsFromNative(info[1])
					?: throw invalidNativeResult("transform", "unknown applied repair flags ${info[1]}"),
			)
		}

		override fun transform(
			sample: DoviSample,
			request: DoviTransformRequest,
			buffer: DoviTransformBuffer,
		): DoviTransformBufferResult {
			var capacity = buffer.bytes.size.coerceAtLeast(sample.bytesSize)
			repeat(MAX_OUTPUT_ATTEMPTS) {
				buffer.ensureCapacity(capacity)
				buffer.outputSize[0] = buffer.bytes.size.toLong()
				when (val status = statusFromNative(
					nativeTransformSample(
						sample.bytes,
						sample.bytesOffset,
						sample.bytesSize,
						framingToNative(sample.framing),
						sample.nalLengthSize,
						presentationToNative(sample.sourceBasePresentation),
						sample.supplementalRpu,
						sample.supplementalRpuOffset,
						sample.supplementalRpuSize,
						targetToNative(request.target),
						repairsToNative(request.repairs),
						buffer.bytes,
						buffer.outputSize,
						buffer.info,
					),
					"transform",
				)) {
					DoviStatus.OK -> return DoviTransformBufferResult(
						bytes = buffer.bytes,
						bytesSize = checkedOutputSize(buffer.outputSize[0], buffer.bytes.size, "transform"),
						input = presentationFromNative(buffer.info[5], "transform"),
						output = presentationFromNative(buffer.info[0], "transform"),
						appliedRepairs = repairsFromNative(buffer.info[1])
							?: throw invalidNativeResult("transform", "unknown applied repair flags ${buffer.info[1]}"),
					)
					DoviStatus.OUTPUT_TOO_SMALL -> {
						capacity = checkedRequiredCapacity(buffer.outputSize[0], buffer.bytes.size, "transform")
					}
					else -> throw DoviException(status, "transform")
				}
			}
			throw invalidNativeResult("transform", "output capacity did not converge")
		}

		override fun writeAv1T35(
			rpu: ByteArray,
			format: DoviRpuFormat,
			completeObu: Boolean,
		): ByteArray {
			require(rpu.isNotEmpty()) { "RPU bytes must not be empty" }
			return growOutput("writeAv1T35", rpu.size) { output, outputSize ->
				nativeWriteAv1T35(rpu, rpuFormatToNative(format), completeObu, output, outputSize)
			}
		}

		override fun capabilities(): Set<DoviCapability> =
			capabilitiesFromNative(nativeCapabilities())

		override fun setMpvRequest(request: DoviTransformRequest?): DoviMpvSession {
			val generation = LongArray(1)
			val status = if (request == null) {
				nativeSetMpvRequest(CLEAR_MPV_REQUEST, 0, generation)
			} else {
				nativeSetMpvRequest(
					targetToNative(request.target),
					repairsToNative(request.repairs),
					generation,
				)
			}
			checkStatus("setMpvRequest", status)
			return NativeDoviMpvSession(validMpvGeneration(generation[0], "setMpvRequest"))
		}

		override fun resetMpvError(session: DoviMpvSession) =
			nativeResetMpvError(nativeGeneration(session, "resetMpvError"))

		override fun consumeMpvError(session: DoviMpvSession): DoviStatus =
			statusFromNative(
				nativeConsumeMpvError(nativeGeneration(session, "consumeMpvError")),
				"consumeMpvError",
			)

		override fun getMpvTransformObservation(session: DoviMpvSession): DoviTransformObservation? {
			if (DoviCapability.MPV_TRANSFORM_OBSERVATION !in capabilities()) return null
			val info = IntArray(2)
			return when (val result = nativeGetMpvTransformObservation(
				nativeGeneration(session, "getMpvTransformObservation"),
				info,
			)) {
				0 -> null
				1 -> DoviTransformObservation(
					input = presentationFromNative(info[0], "getMpvTransformObservation"),
					output = presentationFromNative(info[1], "getMpvTransformObservation"),
				)
				else -> throw invalidNativeResult(
					"getMpvTransformObservation",
					"invalid availability result $result",
				)
			}
		}

		private fun nativeGeneration(session: DoviMpvSession, operation: String): Long =
			(session as? NativeDoviMpvSession)?.generation
				?: throw DoviException(DoviStatus.INVALID_ARGUMENT, operation, "foreign MPV session")

		private fun validMpvGeneration(value: Long, operation: String): Long {
			if (value <= 0) throw invalidNativeResult(operation, "invalid MPV session generation $value")
			return value
		}

		private fun checkStatus(operation: String, value: Int) {
			val status = statusFromNative(value, operation)
			if (status != DoviStatus.OK) throw DoviException(status, operation)
		}

		private fun growOutput(
			operation: String,
			initialCapacity: Int,
			call: (ByteArray, LongArray) -> Int,
		): ByteArray {
			var capacity = initialCapacity.coerceAtLeast(1)
			repeat(MAX_OUTPUT_ATTEMPTS) {
				val output = ByteArray(capacity)
				val outputSize = longArrayOf(capacity.toLong())
				when (val status = statusFromNative(call(output, outputSize), operation)) {
					DoviStatus.OK -> return output.copyOf(
						checkedOutputSize(outputSize[0], capacity, operation),
					)
					DoviStatus.OUTPUT_TOO_SMALL -> {
						capacity = checkedRequiredCapacity(outputSize[0], capacity, operation)
					}
					else -> throw DoviException(status, operation)
				}
			}
			throw invalidNativeResult(operation, "output capacity did not converge")
		}

		private fun checkedOutputSize(value: Long, capacity: Int, operation: String): Int {
			if (value < 0 || value > capacity) {
				throw invalidNativeResult(operation, "invalid output size $value for capacity $capacity")
			}
			return value.toInt()
		}

		private fun checkedRequiredCapacity(value: Long, capacity: Int, operation: String): Int {
			if (value <= capacity || value > Int.MAX_VALUE) {
				throw invalidNativeResult(operation, "invalid required output capacity $value")
			}
			return value.toInt()
		}

		private fun statusFromNative(value: Int, operation: String): DoviStatus = when (value) {
			0 -> DoviStatus.OK
			-1 -> DoviStatus.INVALID_ARGUMENT
			-2 -> DoviStatus.MALFORMED_SAMPLE
			-3 -> DoviStatus.OUTPUT_TOO_SMALL
			-4 -> DoviStatus.RPU_PARSE_FAILED
			-5 -> DoviStatus.RPU_CONVERT_FAILED
			-6 -> DoviStatus.RPU_WRITE_FAILED
			-7 -> DoviStatus.UNSUPPORTED_FRAMING
			-8 -> DoviStatus.INTERNAL_ERROR
			-9 -> DoviStatus.RPU_NOT_FOUND
			-10 -> DoviStatus.UNSUPPORTED_PROFILE
			-11 -> DoviStatus.REENCODE_REQUIRED
			-12 -> DoviStatus.REPAIR_FAILED
			-13 -> DoviStatus.INCONSISTENT_RPU
			else -> throw invalidNativeResult(operation, "unknown status $value")
		}

		private fun framingToNative(value: DoviFraming): Int = when (value) {
			DoviFraming.AUTO -> 0
			DoviFraming.ANNEX_B -> 1
			DoviFraming.LENGTH_PREFIXED -> 2
		}

		private fun framingFromNative(value: Int, operation: String): DoviFraming = when (value) {
			0 -> DoviFraming.AUTO
			1 -> DoviFraming.ANNEX_B
			2 -> DoviFraming.LENGTH_PREFIXED
			else -> throw invalidNativeResult(operation, "unknown framing $value")
		}

		private fun presentationToNative(value: DoviPresentation): Int = when (value) {
			DoviPresentation.UNKNOWN -> 0
			DoviPresentation.PROFILE_5 -> 1
			DoviPresentation.PROFILE_7_MEL -> 2
			DoviPresentation.PROFILE_7_FEL -> 3
			DoviPresentation.PROFILE_8_1 -> 4
			DoviPresentation.PROFILE_8_4 -> 5
			DoviPresentation.HDR10 -> 6
			DoviPresentation.HDR10_PLUS -> 7
			DoviPresentation.HLG -> 8
		}

		private fun presentationFromNative(value: Int, operation: String): DoviPresentation = when (value) {
			0 -> DoviPresentation.UNKNOWN
			1 -> DoviPresentation.PROFILE_5
			2 -> DoviPresentation.PROFILE_7_MEL
			3 -> DoviPresentation.PROFILE_7_FEL
			4 -> DoviPresentation.PROFILE_8_1
			5 -> DoviPresentation.PROFILE_8_4
			6 -> DoviPresentation.HDR10
			7 -> DoviPresentation.HDR10_PLUS
			8 -> DoviPresentation.HLG
			else -> throw invalidNativeResult(operation, "unknown presentation $value")
		}

		private fun targetToNative(value: DoviTarget): Int = when (value) {
			DoviTarget.LOSSLESS_REWRITE -> 0
			DoviTarget.MEL -> 1
			DoviTarget.PROFILE_8_1 -> 2
			DoviTarget.PROFILE_8_1_PRESERVE_MAPPING -> 3
			DoviTarget.PROFILE_8_4 -> 4
			DoviTarget.SOURCE_BASE_PRESENTATION -> 5
		}

		private fun repairsToNative(values: Set<DoviRepair>): Int = values.fold(0) { flags, repair ->
			flags or when (repair) {
				DoviRepair.REMOVE_MAPPING -> 1 shl 0
				DoviRepair.ZERO_ACTIVE_AREA -> 1 shl 1
				DoviRepair.ADD_CMV40_SAFE_DEFAULTS -> 1 shl 2
				DoviRepair.REMOVE_CMV40 -> 1 shl 3
			}
		}

		private fun repairsFromNative(flags: Int): Set<DoviRepair>? {
			if (flags and KNOWN_REPAIR_FLAGS.inv() != 0) return null
			return buildSet {
				if (flags and (1 shl 0) != 0) add(DoviRepair.REMOVE_MAPPING)
				if (flags and (1 shl 1) != 0) add(DoviRepair.ZERO_ACTIVE_AREA)
				if (flags and (1 shl 2) != 0) add(DoviRepair.ADD_CMV40_SAFE_DEFAULTS)
				if (flags and (1 shl 3) != 0) add(DoviRepair.REMOVE_CMV40)
			}
		}

		private fun rpuFormatToNative(value: DoviRpuFormat): Int = when (value) {
			DoviRpuFormat.RAW -> 0
			DoviRpuFormat.UNSPEC62_NAL -> 1
		}

		private fun capabilitiesFromNative(flags: Long): Set<DoviCapability> {
			if (flags and KNOWN_CAPABILITY_FLAGS.inv() != 0L) {
				throw invalidNativeResult("capabilities", "unknown capability flags $flags")
			}
			return buildSet {
				if (flags and (1L shl 0) != 0L) add(DoviCapability.INSPECT)
				if (flags and (1L shl 1) != 0L) add(DoviCapability.VALIDATE)
				if (flags and (1L shl 2) != 0L) add(DoviCapability.LOSSLESS_REWRITE)
				if (flags and (1L shl 3) != 0L) add(DoviCapability.MEL)
				if (flags and (1L shl 4) != 0L) add(DoviCapability.PROFILE_8_1)
				if (flags and (1L shl 5) != 0L) add(DoviCapability.PROFILE_8_1_PRESERVE_MAPPING)
				if (flags and (1L shl 6) != 0L) add(DoviCapability.PROFILE_8_4)
				if (flags and (1L shl 7) != 0L) add(DoviCapability.SOURCE_BASE_PRESENTATION)
				if (flags and (1L shl 8) != 0L) add(DoviCapability.REPAIR_REMOVE_MAPPING)
				if (flags and (1L shl 9) != 0L) add(DoviCapability.REPAIR_ZERO_ACTIVE_AREA)
				if (flags and (1L shl 10) != 0L) add(DoviCapability.REPAIR_ADD_CMV40_SAFE_DEFAULTS)
				if (flags and (1L shl 11) != 0L) add(DoviCapability.REPAIR_REMOVE_CMV40)
				if (flags and (1L shl 12) != 0L) add(DoviCapability.AV1_T35)
				if (flags and (1L shl 13) != 0L) add(DoviCapability.MPV_STATE)
				if (flags and (1L shl 14) != 0L) add(DoviCapability.MPV_TRANSFORM_OBSERVATION)
			}
		}

		private fun nonNegative(value: Int, field: String, operation: String): Int {
			if (value < 0) throw invalidNativeResult(operation, "negative $field $value")
			return value
		}

		private fun validReturnedNalLengthSize(value: Int, operation: String): Int {
			if (value != 0 && value != 1 && value != 2 && value != 4) {
				throw invalidNativeResult(operation, "invalid NAL length size $value")
			}
			return value
		}

		private fun inspectionFlag(flags: Int, flag: Int): Boolean {
			if (flags and KNOWN_INSPECTION_FLAGS.inv() != 0) {
				throw invalidNativeResult("inspect", "unknown inspection flags $flags")
			}
			return flags and flag != 0
		}

		private fun invalidNativeResult(operation: String, detail: String): DoviException =
			DoviException(DoviStatus.INTERNAL_ERROR, operation, detail)

		private const val CLEAR_MPV_REQUEST = -1
		private const val INSPECTION_FIELD_COUNT = 7
		private const val KNOWN_INSPECTION_FLAGS = (1 shl 2) - 1
		private const val TRANSFORM_FIELD_COUNT = 6
		private const val MAX_OUTPUT_ATTEMPTS = 4
		private const val KNOWN_REPAIR_FLAGS = (1 shl 4) - 1
		private const val KNOWN_CAPABILITY_FLAGS = (1L shl 15) - 1L
	}

	private external fun nativeAbiVersion(): Int
	private external fun nativeCapabilities(): Long
	private external fun nativeInspectSample(
		input: ByteArray,
		inputOffset: Int,
		inputSize: Int,
		framing: Int,
		nalLengthSize: Int,
		sourceBasePresentation: Int,
		supplementalRpu: ByteArray?,
		supplementalRpuOffset: Int,
		supplementalRpuSize: Int,
		info: IntArray,
	): Int
	private external fun nativeTransformSample(
		input: ByteArray,
		inputOffset: Int,
		inputSize: Int,
		framing: Int,
		nalLengthSize: Int,
		sourceBasePresentation: Int,
		supplementalRpu: ByteArray?,
		supplementalRpuOffset: Int,
		supplementalRpuSize: Int,
		target: Int,
		repairFlags: Int,
		output: ByteArray,
		outputSize: LongArray,
		info: IntArray,
	): Int
	private external fun nativeTransformSampleAllocated(
		input: ByteArray,
		inputOffset: Int,
		inputSize: Int,
		framing: Int,
		nalLengthSize: Int,
		sourceBasePresentation: Int,
		supplementalRpu: ByteArray?,
		supplementalRpuOffset: Int,
		supplementalRpuSize: Int,
		target: Int,
		repairFlags: Int,
		status: IntArray,
		info: IntArray,
	): ByteArray?
	private external fun nativeWriteAv1T35(
		rpu: ByteArray,
		rpuFormat: Int,
		completeObu: Boolean,
		output: ByteArray,
		outputSize: LongArray,
	): Int
	private external fun nativeSetMpvRequest(target: Int, repairFlags: Int, generation: LongArray): Int
	private external fun nativeResetMpvError(generation: Long)
	private external fun nativeConsumeMpvError(generation: Long): Int
	private external fun nativeGetMpvTransformObservation(generation: Long, info: IntArray): Int
}

internal fun exactTransformResult(
	bytes: ByteArray,
	input: DoviPresentation,
	output: DoviPresentation,
	appliedRepairs: Set<DoviRepair>,
): DoviTransformResult {
	require(bytes.isNotEmpty()) { "Transformed sample bytes must not be empty" }
	return DoviTransformResult(bytes, output, appliedRepairs, input)
}

/** Semantic-only seam used by JVM tests without loading the Android JNI library. */
internal interface DoviBackend {
	fun initialize()
	fun inspect(sample: DoviSample): DoviInspection
	fun transform(sample: DoviSample, request: DoviTransformRequest): DoviTransformResult
	fun transform(
		sample: DoviSample,
		request: DoviTransformRequest,
		buffer: DoviTransformBuffer,
	): DoviTransformBufferResult {
		val result = transform(sample, request)
		buffer.ensureCapacity(result.bytes.size)
		System.arraycopy(result.bytes, 0, buffer.bytes, 0, result.bytes.size)
		return DoviTransformBufferResult(
			buffer.bytes,
			result.bytes.size,
			result.output,
			result.appliedRepairs,
			result.input,
		)
	}
	fun writeAv1T35(rpu: ByteArray, format: DoviRpuFormat, completeObu: Boolean): ByteArray
	fun capabilities(): Set<DoviCapability>
	fun setMpvRequest(request: DoviTransformRequest?): DoviMpvSession
	fun resetMpvError(session: DoviMpvSession)
	fun consumeMpvError(session: DoviMpvSession): DoviStatus
	fun getMpvTransformObservation(session: DoviMpvSession): DoviTransformObservation?
}

internal class DoviApi(
	private val backend: DoviBackend,
) {
	private val availability: Result<Unit> by lazy(LazyThreadSafetyMode.SYNCHRONIZED) {
		runCatching { backend.initialize() }
	}

	fun isAvailable(): Boolean = availability.isSuccess

	fun inspect(sample: DoviSample): DoviInspection = available("inspect") {
		backend.inspect(sample)
	}

	fun transform(sample: DoviSample, request: DoviTransformRequest): DoviTransformResult =
		available("transform") { backend.transform(sample, request) }

	fun transform(
		sample: DoviSample,
		request: DoviTransformRequest,
		buffer: DoviTransformBuffer,
	): DoviTransformBufferResult = available("transform") { backend.transform(sample, request, buffer) }

	fun openTransformSession(
		request: DoviTransformRequest,
		strategy: DoviTransformStrategy = DoviTransformStrategy.LIBDOVI,
		state: DoviTransformSessionState = DoviTransformSessionState(),
	): DoviTransformSession = DoviTransformSession(request, strategy, state) { sample, buffer ->
		transform(sample, request, buffer)
	}

	fun writeAv1T35(rpu: ByteArray, format: DoviRpuFormat, completeObu: Boolean): ByteArray =
		available("writeAv1T35") { backend.writeAv1T35(rpu, format, completeObu) }

	fun capabilities(): Set<DoviCapability> = available("capabilities") {
		backend.capabilities()
	}

	fun setMpvRequest(request: DoviTransformRequest?): DoviMpvSession = available("setMpvRequest") {
		backend.setMpvRequest(request)
	}

	fun resetMpvError(session: DoviMpvSession) = available("resetMpvError") {
		backend.resetMpvError(session)
	}

	fun consumeMpvError(session: DoviMpvSession): DoviStatus = available("consumeMpvError") {
		backend.consumeMpvError(session)
	}

	fun getMpvTransformObservation(session: DoviMpvSession): DoviTransformObservation? =
		available("getMpvTransformObservation") { backend.getMpvTransformObservation(session) }

	private inline fun <T> available(operation: String, block: () -> T): T {
		availability.exceptionOrNull()?.let { throw DoviUnavailableException(operation, it) }
		return block()
	}
}
