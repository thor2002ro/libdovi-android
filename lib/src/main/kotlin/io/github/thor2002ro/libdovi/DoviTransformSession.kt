package io.github.thor2002ro.libdovi

import java.util.concurrent.atomic.AtomicBoolean

enum class DoviTransformStrategy {
	LIBDOVI,
	FAST_SOURCE_BASE_FALLBACK,
}

enum class DoviTransformProcessor {
	LIBDOVI,
	FAST_HDR_BASE,
}

/** Shared fallback state for transform sessions belonging to one playback item. */
class DoviTransformSessionState {
	private val fastRejected = AtomicBoolean(false)

	internal fun allowsFastSourceBase(): Boolean = !fastRejected.get()
	internal fun rejectFastSourceBase() = fastRejected.set(true)
}

class DoviTransformSession internal constructor(
	request: DoviTransformRequest,
	private val strategy: DoviTransformStrategy,
	private val state: DoviTransformSessionState,
	private val transformWithLibdovi: (DoviSample, DoviTransformBuffer) -> DoviTransformBufferResult,
) {
	private var fastEnabled = false
	private var validatedOutput = DoviPresentation.UNKNOWN
	var processor = DoviTransformProcessor.LIBDOVI
		private set

	init {
		require(
			strategy != DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK ||
				request.target == DoviTarget.SOURCE_BASE_PRESENTATION,
		) { "Fast source-base fallback requires a source-base transform request" }
	}

	fun transform(sample: DoviSample, buffer: DoviTransformBuffer): DoviTransformBufferResult {
		if (fastEnabled && state.allowsFastSourceBase()) {
			try {
				return fastSourceBase(sample, buffer).also {
					processor = DoviTransformProcessor.FAST_HDR_BASE
				}
			} catch (_: FastSourceBaseException) {
				fastEnabled = false
				state.rejectFastSourceBase()
			}
		}

		val result = transformWithLibdovi(sample, buffer)
		if (state.allowsFastSourceBase() && strategy == DoviTransformStrategy.FAST_SOURCE_BASE_FALLBACK &&
			result.input == DoviPresentation.PROFILE_8_1 &&
			result.output == sample.sourceBasePresentation &&
			result.output in FAST_SOURCE_BASE_PRESENTATIONS
		) {
			validatedOutput = result.output
			fastEnabled = true
			processor = DoviTransformProcessor.FAST_HDR_BASE
			return result
		}
		processor = DoviTransformProcessor.LIBDOVI
		return result
	}

	private fun fastSourceBase(
		sample: DoviSample,
		buffer: DoviTransformBuffer,
	): DoviTransformBufferResult {
		if (sample.framing != DoviFraming.ANNEX_B || sample.sourceBasePresentation != validatedOutput) {
			throw FastSourceBaseException()
		}
		buffer.ensureCapacity(sample.bytesSize)
		val input = sample.bytes
		val end = sample.bytesOffset + sample.bytesSize
		var position = sample.bytesOffset
		var outputSize = 0

		while (position < end) {
			val prefixSize = startCodeLength(input, position, end)
			if (prefixSize == 0) throw FastSourceBaseException()
			val nalStart = position + prefixSize
			var nextPosition = nalStart
			while (nextPosition < end && startCodeLength(input, nextPosition, end) == 0) nextPosition++
			if (nextPosition - nalStart < 2) throw FastSourceBaseException()
			val first = input[nalStart].toInt() and 0xff
			val second = input[nalStart + 1].toInt() and 0xff
			val type = (first ushr 1) and 0x3f
			val layer = ((first and 1) shl 5) or ((second ushr 3) and 0x1f)
			if (type != 62 && type != 63 && layer == 0) {
				val length = nextPosition - position
				System.arraycopy(input, position, buffer.bytes, outputSize, length)
				outputSize += length
			}
			position = nextPosition
		}
		if (outputSize == 0) throw FastSourceBaseException()

		return DoviTransformBufferResult(
			bytes = buffer.bytes,
			bytesSize = outputSize,
			output = validatedOutput,
			appliedRepairs = emptySet(),
			input = DoviPresentation.PROFILE_8_1,
		)
	}

	private fun startCodeLength(input: ByteArray, position: Int, end: Int): Int = when {
		position + 3 < end && input[position] == 0.toByte() && input[position + 1] == 0.toByte() &&
			input[position + 2] == 0.toByte() && input[position + 3] == 1.toByte() -> 4
		position + 2 < end && input[position] == 0.toByte() && input[position + 1] == 0.toByte() &&
			input[position + 2] == 1.toByte() -> 3
		else -> 0
	}

	private class FastSourceBaseException : RuntimeException()

	private companion object {
		val FAST_SOURCE_BASE_PRESENTATIONS = setOf(DoviPresentation.HDR10, DoviPresentation.HDR10_PLUS)
	}
}
