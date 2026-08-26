package io.github.thor2002ro.libdovi

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
}

private class FakeDoviBackend(
	private val initializeFailure: Throwable? = null,
) : DoviBackend {
	var initializeCalls = 0
	var inspectCalls = 0
	var capabilityCalls = 0
	var resetMpvErrorCalls = 0

	var lastInspectedSample: DoviSample? = null
	var lastTransformRequest: DoviTransformRequest? = null
	var lastRpuFormat: DoviRpuFormat? = null
	var lastMpvRequest: DoviTransformRequest? = null
	var lastResetMpvSession: DoviMpvSession? = null
	var lastConsumedMpvSession: DoviMpvSession? = null
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
	)
	var av1Result = byteArrayOf(1)
	var capabilityResult: Set<DoviCapability> = setOf(DoviCapability.INSPECT)
	var consumedMpvStatus = DoviStatus.OK

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
}

private data class FakeMpvSession(val ordinal: Long) : DoviMpvSession
