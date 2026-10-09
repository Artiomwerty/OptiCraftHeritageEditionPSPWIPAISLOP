#pragma once

// PspFramePacer — locks presentation to exactly 30 fps on the PSP's 59.94 Hz display.
//
// The display runs at ~60 Hz; presenting every 2nd vertical blank gives an even
// 30 fps with no tearing and no frame-time judder (a free-running limiter based on
// milliseconds beats against the 16.7 ms refresh and produces uneven frames).
//
// Pure logic, driven by sceDisplayGetVcount() (the running vblank counter), so it
// is unit-tested on a PC (tests/PspFramePacerTests.cpp).
//
// Per frame:
//     unsigned n = pacer.waitsNeeded(sceDisplayGetVcount());
//     while (n--) sceDisplayWaitVblankStart();
//     pacer.presented(sceDisplayGetVcount());
//     sceGuSwapBuffers();
//
// A frame that takes longer than the budget is NOT dropped to 20 fps: the pacer
// presents at the very next vblank (no extra delay) and counts it as a slow frame,
// so the game can react (see slowFramesInARow()).

class PspFramePacer
{
public:
	explicit PspFramePacer(unsigned vblanksPerFrame = 2) : m_n(vblanksPerFrame ? vblanksPerFrame : 1) {}

	// How many vblank waits to perform before presenting. The counter wraps, so
	// all arithmetic is unsigned subtraction.
	unsigned waitsNeeded(unsigned vcountNow) const
	{
		if (!m_started)
			return 1;
		const unsigned elapsed = vcountNow - m_lastPresent;
		return elapsed >= m_n ? 1u : m_n - elapsed;
	}

	// Call with the vblank counter reached once the waits are done.
	void presented(unsigned vcountAfter)
	{
		if (m_started)
		{
			const unsigned interval = vcountAfter - m_lastPresent;
			if (interval > m_n)
			{
				++m_slowFrames;
				++m_slowInARow;
				if (interval > m_worstInterval) m_worstInterval = interval;
			}
			else
			{
				m_slowInARow = 0;
			}
			++m_frames;
		}
		m_lastPresent = vcountAfter;
		m_started = true;
	}

	unsigned frames() const { return m_frames; }
	unsigned slowFrames() const { return m_slowFrames; }
	unsigned slowFramesInARow() const { return m_slowInARow; }
	unsigned worstIntervalVblanks() const { return m_worstInterval; }
	unsigned vblanksPerFrame() const { return m_n; }

	// Fraction of frames that hit the lock exactly, 0..1 (1.0 = perfect lock).
	float lockedRatio() const
	{
		return m_frames ? 1.0f - static_cast<float>(m_slowFrames) / static_cast<float>(m_frames) : 1.0f;
	}

private:
	unsigned m_n;
	unsigned m_lastPresent = 0;
	unsigned m_frames = 0;
	unsigned m_slowFrames = 0;
	unsigned m_slowInARow = 0;
	unsigned m_worstInterval = 0;
	bool m_started = false;
};
