package io.github.thor2002ro.libdovi

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertSame
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class DoviModelsTest {
	@Test
	fun `ABI version 3 accepts only the matching native contract`() {
		verifyDoviAbiVersion(3)
		assertThrows(IllegalStateException::class.java) { verifyDoviAbiVersion(1) }
		assertThrows(IllegalStateException::class.java) { verifyDoviAbiVersion(2) }
		assertThrows(IllegalStateException::class.java) { verifyDoviAbiVersion(4) }
	}

	@Test
	fun `semantic values cross the test seam without raw ABI values`() {
		val backend = FakeDoviBackend()
		val api = DoviApi(backend)

		DoviFraming.entries.forEach { framing ->
			val sample = sample(
				framing = framing,
				nalLengthSize = if (framing == DoviFraming.LENGTH_PREFIXED) 4 else 0,
			)
			api.inspect(sample)
			assertEquals(sample, backend.lastInspectedSample)
		}
		DoviPresentation.entries.forEach { presentation ->
			backend.inspectionResult = backend.inspectionResult.copy(input = presentation)
			assertEquals(presentation, api.inspect(sample()).input)
		}
		DoviTarget.entries.forEach { target ->
			val request = DoviTransformRequest(target)
			api.transform(sample(), request)
			assertEquals(request, backend.lastTransformRequest)
		}
		DoviRepair.entries.forEach { repair ->
			val request = DoviTransformRequest(DoviTarget.PROFILE_8_1, setOf(repair))
			api.transform(sample(), request)
			assertEquals(request, backend.lastTransformRequest)
		}
		DoviRpuFormat.entries.forEach { format ->
			api.writeAv1T35(byteArrayOf(1), format, completeObu = true)
			assertEquals(format, backend.lastRpuFormat)
		}

		backend.capabilityResult = DoviCapability.entries.toSet()
		assertEquals(DoviCapability.entries.toSet(), api.capabilities())
		DoviStatus.entries.forEach { status ->
			backend.consumedMpvStatus = status
			assertEquals(status, api.consumeMpvError(FakeMpvSession(1)))
		}
	}

	@Test
	fun `request rejects incompatible repairs`() {
		assertThrows(IllegalArgumentException::class.java) {
			DoviTransformRequest(
				target = DoviTarget.PROFILE_8_1,
				repairs = setOf(
					DoviRepair.ADD_CMV40_SAFE_DEFAULTS,
					DoviRepair.REMOVE_CMV40,
				),
			)
		}
		assertThrows(IllegalArgumentException::class.java) {
			DoviTransformRequest(
				target = DoviTarget.SOURCE_BASE_PRESENTATION,
				repairs = setOf(DoviRepair.ZERO_ACTIVE_AREA),
			)
		}
	}

	@Test
	fun `request snapshots caller repair set`() {
		val callerRepairs = mutableSetOf(DoviRepair.ADD_CMV40_SAFE_DEFAULTS)
		val request = DoviTransformRequest(DoviTarget.PROFILE_8_1, callerRepairs)
		val equalRequest = DoviTransformRequest(
			DoviTarget.PROFILE_8_1,
			setOf(DoviRepair.ADD_CMV40_SAFE_DEFAULTS),
		)

		callerRepairs += DoviRepair.REMOVE_CMV40

		assertEquals(setOf(DoviRepair.ADD_CMV40_SAFE_DEFAULTS), request.repairs)
		assertEquals(equalRequest, request)
		assertEquals(equalRequest.hashCode(), request.hashCode())
	}

	@Test
	fun `request copy and destructuring preserve value model conveniences`() {
		val original = DoviTransformRequest(
			target = DoviTarget.MEL,
			repairs = setOf(DoviRepair.ZERO_ACTIVE_AREA),
		)
		val equalCopy = original.copy()
		val retargeted = original.copy(target = DoviTarget.PROFILE_8_1)
		val (target, repairs) = original

		assertEquals(original, equalCopy)
		assertEquals(original.hashCode(), equalCopy.hashCode())
		assertEquals(DoviTarget.MEL, target)
		assertEquals(setOf(DoviRepair.ZERO_ACTIVE_AREA), repairs)
		assertEquals(DoviTarget.PROFILE_8_1, retargeted.target)
		assertEquals(original.repairs, retargeted.repairs)
		assertEquals(
			"DoviTransformRequest(target=MEL, repairs=[ZERO_ACTIVE_AREA])",
			original.toString(),
		)
	}

	@Test
	fun `request copy snapshots replacement repair set`() {
		val replacementRepairs = mutableSetOf(DoviRepair.ADD_CMV40_SAFE_DEFAULTS)
		val copied = DoviTransformRequest(DoviTarget.MEL).copy(
			target = DoviTarget.PROFILE_8_1,
			repairs = replacementRepairs,
		)

		replacementRepairs += DoviRepair.REMOVE_CMV40

		assertEquals(DoviTarget.PROFILE_8_1, copied.target)
		assertEquals(setOf(DoviRepair.ADD_CMV40_SAFE_DEFAULTS), copied.repairs)
	}

	@Test
	fun `sample validates base presentation and NAL length size`() {
		assertThrows(IllegalArgumentException::class.java) {
			DoviSample(
				bytes = byteArrayOf(1),
				framing = DoviFraming.LENGTH_PREFIXED,
				nalLengthSize = 3,
			)
		}
		assertThrows(IllegalArgumentException::class.java) {
			DoviSample(
				bytes = byteArrayOf(1),
				sourceBasePresentation = DoviPresentation.PROFILE_7_FEL,
			)
		}

		listOf(1, 2, 4).forEach { lengthSize ->
			DoviSample(
				bytes = byteArrayOf(1),
				framing = DoviFraming.LENGTH_PREFIXED,
				nalLengthSize = lengthSize,
			)
		}
	}

	@Test
	fun `sample validates reusable input ranges`() {
		val bytes = byteArrayOf(9, 1, 2, 3, 9)
		val supplemental = byteArrayOf(8, 4, 5, 8)
		val sample = DoviSample(
			bytes = bytes,
			supplementalRpu = supplemental,
			bytesOffset = 1,
			bytesSize = 3,
			supplementalRpuOffset = 1,
			supplementalRpuSize = 2,
		)

		assertSame(bytes, sample.bytes)
		assertSame(supplemental, sample.supplementalRpu)
		assertEquals(1, sample.bytesOffset)
		assertEquals(3, sample.bytesSize)
		assertEquals(1, sample.supplementalRpuOffset)
		assertEquals(2, sample.supplementalRpuSize)
		assertThrows(IllegalArgumentException::class.java) {
			DoviSample(bytes = bytes, bytesOffset = 4, bytesSize = 2)
		}
		assertThrows(IllegalArgumentException::class.java) {
			DoviSample(bytes = bytes, supplementalRpuSize = 1)
		}
	}

	@Test
	fun `unavailable library is cached without invoking an operation`() {
		val backend = FakeDoviBackend(initializeFailure = UnsatisfiedLinkError("missing"))
		val api = DoviApi(backend)

		assertFalse(api.isAvailable())
		assertFalse(api.isAvailable())
		val exception = assertThrows(DoviUnavailableException::class.java) {
			api.inspect(sample())
		}
		assertEquals("inspect", exception.operation)
		assertEquals(1, backend.initializeCalls)
		assertEquals(0, backend.inspectCalls)
	}

	@Test
	fun `successful availability initialization is cached across operations`() {
		val backend = FakeDoviBackend()
		val api = DoviApi(backend)

		assertTrue(api.isAvailable())
		assertTrue(api.isAvailable())
		api.inspect(sample())
		api.capabilities()

		assertEquals(1, backend.initializeCalls)
		assertEquals(1, backend.inspectCalls)
		assertEquals(1, backend.capabilityCalls)
	}

	@Test
	fun `semantic backend failure preserves typed status and operation`() {
		val backend = FakeDoviBackend().apply {
			inspectFailure = DoviException(DoviStatus.REPAIR_FAILED, "inspect")
		}
		val exception = assertThrows(DoviException::class.java) {
			DoviApi(backend).inspect(sample())
		}

		assertEquals(DoviStatus.REPAIR_FAILED, exception.status)
		assertEquals("inspect", exception.operation)
	}

	@Test
	fun `semantic operation results are returned unchanged`() {
		val backend = FakeDoviBackend().apply {
			inspectionResult = DoviInspection(
				input = DoviPresentation.PROFILE_7_FEL,
				rpuCount = 2,
				enhancementNalCount = 3,
				videoNalCount = 4,
				framing = DoviFraming.LENGTH_PREFIXED,
				nalLengthSize = 4,
				mappingPresent = true,
				cmv40Present = true,
			)
			transformResult = DoviTransformResult(
				bytes = byteArrayOf(10, 20, 30),
				output = DoviPresentation.PROFILE_8_1,
				appliedRepairs = setOf(DoviRepair.REMOVE_MAPPING),
				input = DoviPresentation.PROFILE_7_FEL,
			)
			av1Result = byteArrayOf(1, 2, 3)
		}
		val api = DoviApi(backend)

		assertEquals(backend.inspectionResult, api.inspect(sample()))
		val transformed = api.transform(
			sample(),
			DoviTransformRequest(DoviTarget.PROFILE_8_1),
		)
		assertArrayEquals(backend.transformResult.bytes, transformed.bytes)
		assertEquals(backend.transformResult.output, transformed.output)
		assertEquals(backend.transformResult.appliedRepairs, transformed.appliedRepairs)
		assertArrayEquals(
			backend.av1Result,
			api.writeAv1T35(byteArrayOf(9), DoviRpuFormat.RAW, completeObu = false),
		)
	}

	@Test
	fun `streaming transform reuses caller-owned output storage`() {
		val backend = FakeDoviBackend().apply {
			transformResult = DoviTransformResult(
				bytes = byteArrayOf(10, 20, 30),
				output = DoviPresentation.PROFILE_8_1,
				appliedRepairs = emptySet(),
				input = DoviPresentation.PROFILE_7_FEL,
			)
		}
		val api = DoviApi(backend)
		val buffer = DoviTransformBuffer()
		val request = DoviTransformRequest(DoviTarget.PROFILE_8_1)

		val first = api.transform(sample(), request, buffer)
		val second = api.transform(sample(), request, buffer)

		assertSame(first.bytes, second.bytes)
		assertEquals(3, second.bytesSize)
		assertArrayEquals(byteArrayOf(10, 20, 30), second.bytes.copyOf(second.bytesSize))
	}

	@Test
	fun `transform buffer keeps spare capacity for growing video samples`() {
		val buffer = DoviTransformBuffer()
		buffer.ensureCapacity(1_000)
		val bytes = buffer.bytes

		buffer.ensureCapacity(1_001)

		assertSame(bytes, buffer.bytes)
	}

	@Test
	fun `fast source base session validates once then filters Dolby Vision NAL units`() {
		val validation = annexBNal(type = 1, payload = byteArrayOf(0x11))
		val retainedVps = annexBNal(type = 32, payload = byteArrayOf(0x21), startCodeLength = 3)
		val retainedVideo = annexBNal(type = 1, payload = byteArrayOf(0x01, 0x02))
		val droppedRpu = annexBNal(type = 62, payload = byteArrayOf(0x3e))
		val droppedEnhancement = annexBNal(type = 1, layer = 1, payload = byteArrayOf(0x31))
		val droppedUnspecified = annexBNal(type = 63, payload = byteArrayOf(0x3f))
		val backend = FakeDoviBackend().apply {
			transformResult = DoviTransformResult(
				bytes = validation,
				output = DoviPresentation.HDR10,
				appliedRepairs = emptySet(),
				input = DoviPresentation.PROFILE_8_1,
			)
		}
		val session = DoviApi(backend).openTransformSession(
			DoviTransformRequest(DoviTarget.SOURCE_BASE_PRESENTATION),
			DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK,
		)
		val buffer = DoviTransformBuffer()

		session.transform(sourceBaseSample(validation), buffer)
		assertEquals(DoviTransformProcessor.FAST_HDR_BASE, session.processor)
		val second = session.transform(
			sourceBaseSample(retainedVps + droppedRpu + retainedVideo + droppedEnhancement + droppedUnspecified),
			buffer,
		)

		assertEquals(1, backend.transformCalls)
		assertEquals(DoviTransformProcessor.FAST_HDR_BASE, session.processor)
		assertArrayEquals(
			retainedVps + retainedVideo,
			second.bytes.copyOf(second.bytesSize),
		)
		assertEquals(DoviPresentation.PROFILE_8_1, second.input)
		assertEquals(DoviPresentation.HDR10, second.output)
	}

	@Test
	fun `fast source base rejection falls back to libdovi for the remaining session`() {
		val valid = annexBNal(type = 1)
		val malformed = byteArrayOf(1, 2, 3)
		val backend = FakeDoviBackend().apply {
			transformResult = DoviTransformResult(
				bytes = valid,
				output = DoviPresentation.HDR10_PLUS,
				appliedRepairs = emptySet(),
				input = DoviPresentation.PROFILE_8_1,
			)
		}
		val session = DoviApi(backend).openTransformSession(
			DoviTransformRequest(DoviTarget.SOURCE_BASE_PRESENTATION),
			DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK,
		)
		val buffer = DoviTransformBuffer()

		session.transform(sourceBaseSample(valid, DoviPresentation.HDR10_PLUS), buffer)
		backend.transformResult = backend.transformResult.copy(bytes = malformed)
		session.transform(sourceBaseSample(malformed, DoviPresentation.HDR10_PLUS), buffer)
		assertEquals(DoviTransformProcessor.LIBDOVI, session.processor)
		session.transform(sourceBaseSample(valid, DoviPresentation.HDR10_PLUS), buffer)

		assertEquals(3, backend.transformCalls)
		assertEquals(DoviTransformProcessor.LIBDOVI, session.processor)
	}

	@Test
	fun `fast source base rejection is shared by sessions for one playback item`() {
		val valid = annexBNal(type = 1)
		val malformed = byteArrayOf(1, 2, 3)
		val backend = FakeDoviBackend().apply {
			transformResult = DoviTransformResult(
				bytes = valid,
				output = DoviPresentation.HDR10,
				appliedRepairs = emptySet(),
				input = DoviPresentation.PROFILE_8_1,
			)
		}
		val api = DoviApi(backend)
		val state = DoviTransformSessionState()
		val request = DoviTransformRequest(DoviTarget.SOURCE_BASE_PRESENTATION)
		val firstSession = api.openTransformSession(request, DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK, state)
		val buffer = DoviTransformBuffer()

		firstSession.transform(sourceBaseSample(valid), buffer)
		backend.transformResult = backend.transformResult.copy(bytes = malformed)
		firstSession.transform(sourceBaseSample(malformed), buffer)
		val recreatedSession = api.openTransformSession(
			request,
			DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK,
			state,
		)
		backend.transformResult = backend.transformResult.copy(bytes = valid)
		recreatedSession.transform(sourceBaseSample(valid), buffer)
		recreatedSession.transform(sourceBaseSample(valid), buffer)

		assertEquals(4, backend.transformCalls)
		assertEquals(DoviTransformProcessor.LIBDOVI, recreatedSession.processor)
	}

	@Test
	fun `fast source base rejects a changed source presentation`() {
		val valid = annexBNal(type = 1)
		val backend = FakeDoviBackend().apply {
			transformResult = DoviTransformResult(
				bytes = valid,
				output = DoviPresentation.HDR10,
				appliedRepairs = emptySet(),
				input = DoviPresentation.PROFILE_8_1,
			)
		}
		val session = DoviApi(backend).openTransformSession(
			DoviTransformRequest(DoviTarget.SOURCE_BASE_PRESENTATION),
			DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK,
		)
		val buffer = DoviTransformBuffer()

		session.transform(sourceBaseSample(valid, DoviPresentation.HDR10), buffer)
		backend.transformResult = backend.transformResult.copy(output = DoviPresentation.HDR10_PLUS)
		val result = session.transform(sourceBaseSample(valid, DoviPresentation.HDR10_PLUS), buffer)

		assertEquals(2, backend.transformCalls)
		assertEquals(DoviPresentation.HDR10_PLUS, result.output)
		assertEquals(DoviTransformProcessor.LIBDOVI, session.processor)
	}

	@Test
	fun `allocated transform publishes the exact native byte array`() {
		val bytes = byteArrayOf(10, 20, 30)

		val result = exactTransformResult(
			bytes = bytes,
			input = DoviPresentation.PROFILE_7_FEL,
			output = DoviPresentation.PROFILE_8_1,
			appliedRepairs = setOf(DoviRepair.ZERO_ACTIVE_AREA),
		)

		assertSame(bytes, result.bytes)
		assertEquals(DoviPresentation.PROFILE_7_FEL, result.input)
		assertEquals(DoviPresentation.PROFILE_8_1, result.output)
		assertEquals(setOf(DoviRepair.ZERO_ACTIVE_AREA), result.appliedRepairs)
	}

	@Test
	fun `MPV semantic request reset and error operations are delegated`() {
		val backend = FakeDoviBackend().apply {
			consumedMpvStatus = DoviStatus.RPU_CONVERT_FAILED
		}
		val api = DoviApi(backend)
		val request = DoviTransformRequest(
			target = DoviTarget.MEL,
			repairs = setOf(DoviRepair.ZERO_ACTIVE_AREA),
		)

		val firstSession = api.setMpvRequest(request)
		assertEquals(request, backend.lastMpvRequest)
		val clearSession = api.setMpvRequest(null)
		assertEquals(null, backend.lastMpvRequest)
		val firstToken = firstSession as FakeMpvSession
		val clearToken = clearSession as FakeMpvSession
		assertTrue(firstToken.ordinal > 0)
		assertTrue(clearToken.ordinal > firstToken.ordinal)
		api.resetMpvError(firstSession)
		assertEquals(1, backend.resetMpvErrorCalls)
		assertEquals(firstSession, backend.lastResetMpvSession)
		assertEquals(DoviStatus.RPU_CONVERT_FAILED, api.consumeMpvError(firstSession))
		assertEquals(firstSession, backend.lastConsumedMpvSession)
		backend.mpvTransformObservation = DoviTransformObservation(
			input = DoviPresentation.PROFILE_7_FEL,
			output = DoviPresentation.PROFILE_8_1,
		)
		assertEquals(backend.mpvTransformObservation, api.getMpvTransformObservation(firstSession))
		assertEquals(firstSession, backend.lastObservedMpvSession)
	}

	private fun sample(
		framing: DoviFraming = DoviFraming.ANNEX_B,
		nalLengthSize: Int = 0,
	) = DoviSample(
		bytes = byteArrayOf(0, 0, 0, 1),
		framing = framing,
		nalLengthSize = nalLengthSize,
		sourceBasePresentation = DoviPresentation.HDR10,
	)

	private fun sourceBaseSample(
		bytes: ByteArray,
		presentation: DoviPresentation = DoviPresentation.HDR10,
	) = DoviSample(
		bytes = bytes,
		framing = DoviFraming.ANNEX_B,
		sourceBasePresentation = presentation,
	)

	private fun annexBNal(
		type: Int,
		layer: Int = 0,
		payload: ByteArray = byteArrayOf(0x01),
		startCodeLength: Int = 4,
	): ByteArray {
		val prefix = if (startCodeLength == 3) byteArrayOf(0, 0, 1) else byteArrayOf(0, 0, 0, 1)
		val header = byteArrayOf(
			((type shl 1) or ((layer ushr 5) and 1)).toByte(),
			((layer and 0x1f) shl 3).toByte(),
		)
		return prefix + header + payload
	}
}

private class FakeDoviBackend(
	private val initializeFailure: Throwable? = null,
) : DoviBackend {
	var initializeCalls = 0
	var inspectCalls = 0
	var capabilityCalls = 0
	var resetMpvErrorCalls = 0
	var transformCalls = 0

	var lastInspectedSample: DoviSample? = null
	var lastTransformRequest: DoviTransformRequest? = null
	var lastRpuFormat: DoviRpuFormat? = null
	var lastMpvRequest: DoviTransformRequest? = null
	var lastResetMpvSession: DoviMpvSession? = null
	var lastConsumedMpvSession: DoviMpvSession? = null
	var lastObservedMpvSession: DoviMpvSession? = null
	private var nextMpvGeneration = 0L

	var inspectFailure: DoviException? = null
	var inspectionResult = DoviInspection(
		input = DoviPresentation.PROFILE_7_MEL,
		rpuCount = 1,
		enhancementNalCount = 1,
		videoNalCount = 1,
		framing = DoviFraming.ANNEX_B,
		nalLengthSize = 0,
	)
	var transformResult = DoviTransformResult(
		bytes = byteArrayOf(1),
		output = DoviPresentation.PROFILE_8_1,
		appliedRepairs = emptySet(),
		input = DoviPresentation.PROFILE_7_MEL,
	)
	var av1Result = byteArrayOf(1)
	var capabilityResult: Set<DoviCapability> = setOf(DoviCapability.INSPECT)
	var consumedMpvStatus = DoviStatus.OK
	var mpvTransformObservation: DoviTransformObservation? = null

	override fun initialize() {
		initializeCalls++
		initializeFailure?.let { throw it }
	}

	override fun inspect(sample: DoviSample): DoviInspection {
		inspectCalls++
		lastInspectedSample = sample
		inspectFailure?.let { throw it }
		return inspectionResult
	}

	override fun transform(
		sample: DoviSample,
		request: DoviTransformRequest,
	): DoviTransformResult {
		transformCalls++
		lastTransformRequest = request
		return transformResult
	}

	override fun writeAv1T35(
		rpu: ByteArray,
		format: DoviRpuFormat,
		completeObu: Boolean,
	): ByteArray {
		lastRpuFormat = format
		return av1Result
	}

	override fun capabilities(): Set<DoviCapability> {
		capabilityCalls++
		return capabilityResult
	}

	override fun setMpvRequest(request: DoviTransformRequest?): DoviMpvSession {
		lastMpvRequest = request
		return FakeMpvSession(++nextMpvGeneration)
	}

	override fun resetMpvError(session: DoviMpvSession) {
		resetMpvErrorCalls++
		lastResetMpvSession = session
	}

	override fun consumeMpvError(session: DoviMpvSession): DoviStatus {
		lastConsumedMpvSession = session
		return consumedMpvStatus
	}

	override fun getMpvTransformObservation(session: DoviMpvSession): DoviTransformObservation? {
		lastObservedMpvSession = session
		return mpvTransformObservation
	}
}

private data class FakeMpvSession(val ordinal: Long) : DoviMpvSession
