//
// diag.cpp
//
// Diagnostics: error counters, optional log file and on-screen diagnostics
//
// mt32-pi - A baremetal MIDI synthesizer for Raspberry Pi
// Copyright (C) 2026 mt32-pi-nuked contributors
//
// This file is part of mt32-pi.
//
// mt32-pi is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// mt32-pi is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.
//
// You should have received a copy of the GNU General Public License along with
// mt32-pi. If not, see <http://www.gnu.org/licenses/>.
//

#include "diag.h"

#include <circle/bcmpropertytags.h>
#include <circle/cputhrottle.h>
#include <circle/gpiopin.h>
#include <circle/i2cmaster.h>
#include <circle/logger.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/spinlock.h>
#include <circle/util.h>
#include <fatfs/ff.h>

#include <climits>
#include <cmath>
#include <cstdio>

#include "synth/sc55synth.h"
#include "utility.h"

LOGMODULE("diag");

// I2C bus lines (I2C1 on all supported models)
constexpr unsigned GPIOPinSDA = 2;
constexpr unsigned GPIOPinSCL = 3;

namespace Diag
{
	volatile u32 nCore0Loops = 0;
	volatile u32 nCore2Loops = 0;
	volatile u32 nMIDIBytes  = 0;
	volatile u32 nUARTErrors = 0;
	volatile u32 nI2CErrors  = 0;
	volatile u32 nLCDErrors  = 0;

	volatile u64 nIdleTicks[MaxCores] = { 0 };

	// Synth output level since the last LogAudioLevel() (written by the audio task on core 2)
	volatile float fWindowPeak       = 0.0f;
	volatile float fWindowSumSquares = 0.0f;
	volatile u32 nWindowSamples      = 0;
	volatile u32 nWindowOver         = 0;

	// Whole-session statistics per synth
	float fSessionPeak[MaxSynths] = { 0.0f };
	u32 nSessionOver[MaxSynths]   = { 0 };

	void RecordAudioLevel(float fPeak, float fSumSquares, u32 nSamples, u32 nOverSamples)
	{
		if (fPeak > fWindowPeak)
			fWindowPeak = fPeak;
		fWindowSumSquares += fSumSquares;
		nWindowSamples += nSamples;
		nWindowOver += nOverSamples;
	}

	// Format a linear level (1.0 = full scale) as dBFS
	void FormatLevel(char* pBuffer, size_t nSize, float fLevel)
	{
		if (fLevel < 0.00001f)
			snprintf(pBuffer, nSize, "silent");
		else
			snprintf(pBuffer, nSize, "%+.1fdBFS", 20.0f * log10f(fLevel));
	}

	void LogAudioLevel(unsigned nSynth, const char* pSynthName)
	{
		// Read and reset the window; a block recorded in between may be lost (harmless)
		const float fPeak = fWindowPeak;
		const float fRMS  = nWindowSamples ? sqrtf(fWindowSumSquares / nWindowSamples) : 0.0f;
		const u32 nOver   = nWindowOver;
		fWindowPeak       = 0.0f;
		fWindowSumSquares = 0.0f;
		nWindowSamples    = 0;
		nWindowOver       = 0;

		if (nSynth >= MaxSynths)
			return;

		if (fPeak > fSessionPeak[nSynth])
			fSessionPeak[nSynth] = fPeak;
		nSessionOver[nSynth] += nOver;

		char Peak[16], RMS[16], SessionPeak[16];
		FormatLevel(Peak, sizeof(Peak), fPeak);
		FormatLevel(RMS, sizeof(RMS), fRMS);
		FormatLevel(SessionPeak, sizeof(SessionPeak), fSessionPeak[nSynth]);

		LOGNOTE("level %s peak=%s rms=%s over=%u (session: max %s, over %u)",
			pSynthName, Peak, RMS, nOver, SessionPeak, nSessionOver[nSynth]);
	}

	constexpr size_t MaxLineLength = 128;
	constexpr size_t LogRingSize   = 64 * 1024;

	// Protects all log buffers; the logger may be called from any core
	CSpinLock LogLock(IRQ_LEVEL);

	// Line being assembled, and the most recently completed line (for the LCD)
	char LineBuffer[MaxLineLength];
	size_t nLineLength = 0;
	bool bInEscape     = false;
	char LastLine[MaxLineLength] = "(no log yet)";

	// All log text, waiting to be written to the SD card
	char LogRing[LogRingSize];
	size_t nRingWritten = 0;
	size_t nRingFlushed = 0;

	void CommitLine()
	{
		LineBuffer[nLineLength] = '\0';

		// Skip the "hh:mm:ss.cc " timestamp prefix
		const char* pStart = LineBuffer;
		if (nLineLength > 12 && LineBuffer[2] == ':' && LineBuffer[5] == ':' && LineBuffer[8] == '.')
			pStart += 12;

		strncpy(LastLine, pStart, MaxLineLength - 1);
		LastLine[MaxLineLength - 1] = '\0';
		nLineLength = 0;
	}

	void AppendToRing(char chChar)
	{
		LogRing[nRingWritten % LogRingSize] = chChar;
		++nRingWritten;

		// Drop the oldest text if the file writer has fallen behind
		if (nRingWritten - nRingFlushed > LogRingSize)
			nRingFlushed = nRingWritten - LogRingSize;
	}

	void GetLastLogLine(char* pBuffer, size_t nSize)
	{
		LogLock.Acquire();
		strncpy(pBuffer, LastLine, nSize - 1);
		LogLock.Release();
		pBuffer[nSize - 1] = '\0';
	}

	void LogI2CFailure(const char* pWhat, int nResult, u32 nCount)
	{
		// Log the first failures in full, then only every 100th
		if (nCount > 20 && nCount % 100 != 0)
			return;

		const u32 nPins = CGPIOPin::ReadAll();
#if RASPPI >= 5
		LOGWARN("%s failed: %d (#%u) abort=%04x rec=%u/%u sda=%u scl=%u", pWhat, nResult, nCount,
			g_nI2CMasterLastAbortSource, g_nI2CMasterRecoveries, g_nI2CMasterRecoveryFailures,
			(nPins >> GPIOPinSDA) & 1, (nPins >> GPIOPinSCL) & 1);
#else
		LOGWARN("%s failed: %d (#%u) sda=%u scl=%u", pWhat, nResult, nCount,
			(nPins >> GPIOPinSDA) & 1, (nPins >> GPIOPinSCL) & 1);
#endif
	}

	void LogStatus()
	{
		// Temperature, clock and firmware throttling flags (read only; sticky bits are not cleared)
		CCPUThrottle* const pThrottle = CCPUThrottle::Get();
		CBcmPropertyTags Tags;
		TPropertyTagSimple Throttled;
		Throttled.nValue = 0;
		Tags.GetTag(PROPTAG_GET_THROTTLED, &Throttled, sizeof(Throttled), sizeof(Throttled.nValue));

		// Actual ARM clock, which is lower than the set clock while the firmware throttles
		TPropertyTagClockRate MeasuredClock;
		MeasuredClock.nClockId = CLOCK_ID_ARM;
		MeasuredClock.nRate    = 0;
		Tags.GetTag(PROPTAG_GET_CLOCK_RATE_MEASURED, &MeasuredClock, sizeof(MeasuredClock), 4);

		LOGNOTE("heat temp=%uC clock=%u/%uMHz throttled=%05x underruns=%u sc55underruns=%u",
			pThrottle->GetTemperature(), MeasuredClock.nRate / 1000000, pThrottle->GetClockRate() / 1000000,
			Throttled.nValue, g_nSoundBaseDeviceUnderruns, static_cast<unsigned>(SC55GetUnderruns()));

#if AARCH == 64
		// CPU load per core since the last status line: the share of time not spent resting
		static u64 s_nLastCounter = 0;
		static u64 s_nLastIdleTicks[MaxCores] = { 0 };

		u64 nCounter;
		asm volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (nCounter) : : "memory");

		unsigned nBusyPercent[MaxCores] = { 0 };
		const u64 nElapsed = nCounter - s_nLastCounter;
		for (unsigned i = 0; i < MaxCores; ++i)
		{
			const u64 nIdle = nIdleTicks[i];
			if (s_nLastCounter && nElapsed)
			{
				const u64 nIdleDelta = Utility::Min(nIdle - s_nLastIdleTicks[i], nElapsed);
				nBusyPercent[i] = static_cast<unsigned>(100 - nIdleDelta * 100 / nElapsed);
			}
			s_nLastIdleTicks[i] = nIdle;
		}
		s_nLastCounter = nCounter;

		// SC-55 buffer: configured size and lowest level during playback since the last status line
		unsigned nBufferMillis, nLowWaterMillis;
		SC55GetBufferStats(nBufferMillis, nLowWaterMillis);

		char LowWater[16] = "-";
		if (nLowWaterMillis != UINT_MAX)
			snprintf(LowWater, sizeof(LowWater), "%ums", nLowWaterMillis);

		LOGNOTE("load main=%u%% ui=%u%% audio=%u%% sc55=%u%% sc55buffer=%ums low=%s",
			nBusyPercent[0], nBusyPercent[1], nBusyPercent[2], nBusyPercent[3], nBufferMillis, LowWater);
#endif

		const u32 nPins = CGPIOPin::ReadAll();
#if RASPPI >= 5
		LOGNOTE("stat c0=%u c2=%u midi=%u uart=%u mister=%u lcd=%u rec=%u/%u partial=%u err=%d abort=%04x sda=%u scl=%u",
			nCore0Loops, nCore2Loops, nMIDIBytes, nUARTErrors, nI2CErrors, nLCDErrors,
			g_nI2CMasterRecoveries, g_nI2CMasterRecoveryFailures, g_nI2CMasterPartialReads,
			g_nI2CMasterLastError, g_nI2CMasterLastAbortSource,
			(nPins >> GPIOPinSDA) & 1, (nPins >> GPIOPinSCL) & 1);
#else
		LOGNOTE("stat c0=%u c2=%u midi=%u uart=%u mister=%u lcd=%u sda=%u scl=%u",
			nCore0Loops, nCore2Loops, nMIDIBytes, nUARTErrors, nI2CErrors, nLCDErrors,
			(nPins >> GPIOPinSDA) & 1, (nPins >> GPIOPinSCL) & 1);
#endif
	}

	FIL LogFile;
	bool bLogFileOpen = false;

	bool OpenLogFile()
	{
		bLogFileOpen = f_open(&LogFile, "SD:mt32-pi-log.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK;
		return bLogFileOpen;
	}

	void FlushLogFile()
	{
		if (!bLogFileOpen)
			return;

		bool bWritten = false;
		char Chunk[1024];

		while (true)
		{
			size_t nBytes = 0;

			LogLock.Acquire();
			while (nRingFlushed < nRingWritten && nBytes < sizeof(Chunk))
				Chunk[nBytes++] = LogRing[nRingFlushed++ % LogRingSize];
			LogLock.Release();

			if (!nBytes)
				break;

			UINT nWritten;
			if (f_write(&LogFile, Chunk, nBytes, &nWritten) != FR_OK)
			{
				bLogFileOpen = false;
				return;
			}

			bWritten = true;
		}

		// Commit to the card so the log survives a power cut
		if (bWritten)
			f_sync(&LogFile);
	}
}

int CDiagLogDevice::Write(const void* pBuffer, size_t nCount)
{
	const char* pChars = static_cast<const char*>(pBuffer);

	Diag::LogLock.Acquire();

	for (size_t i = 0; i < nCount; ++i)
	{
		const char chChar = pChars[i];

		// Drop ANSI escape sequences (log colours)
		if (Diag::bInEscape)
		{
			if ((chChar >= 'A' && chChar <= 'Z') || (chChar >= 'a' && chChar <= 'z'))
				Diag::bInEscape = false;
			continue;
		}

		if (chChar == '\x1b')
		{
			Diag::bInEscape = true;
			continue;
		}

		Diag::AppendToRing(chChar);

		if (chChar == '\n')
			Diag::CommitLine();
		else if (Diag::nLineLength < Diag::MaxLineLength - 1)
			Diag::LineBuffer[Diag::nLineLength++] = chChar;
	}

	Diag::LogLock.Release();

	if (m_pForwardDevice)
		m_pForwardDevice->Write(pBuffer, nCount);

	return static_cast<int>(nCount);
}
