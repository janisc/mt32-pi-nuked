//
// diag.h
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

#ifndef _diag_h
#define _diag_h

// Diagnostics for troubleshooting without a serial console or HDMI:
// - The system log is buffered in memory; with log_file = on in mt32-pi.cfg it is
//   written to SD:mt32-pi-log.txt, together with a status line every 5 seconds.
// - Building with DIAG=1 (DIAG_LCD) replaces the LCD UI with counters and the
//   latest log message.

#include <circle/device.h>
#include <circle/types.h>

namespace Diag
{
	extern volatile u32 nCore0Loops;
	extern volatile u32 nCore2Loops;
	extern volatile u32 nMIDIBytes;
	extern volatile u32 nUARTErrors;
	extern volatile u32 nI2CErrors;
	extern volatile u32 nLCDErrors;

	// Time each core spent resting (generic timer ticks), for the CPU load in the log
	constexpr unsigned MaxCores = 4;
	extern volatile u64 nIdleTicks[MaxCores];

	// Copy the most recent log line (timestamp and colour codes stripped)
	void GetLastLogLine(char* pBuffer, size_t nSize);

	// Log a failed I2C transfer with error details and bus line levels (rate-limited)
	void LogI2CFailure(const char* pWhat, int nResult, u32 nCount);

	// Log all counters on one line
	void LogStatus();

	// Record the peak level (1.0 = full scale), the sum of the squared samples and the number of
	// samples over full scale of one block of synth output; called by the audio task
	void RecordAudioLevel(float fPeak, float fSumSquares, u32 nSamples, u32 nOverSamples);

	// Log the peak and average (RMS) level of the given synth since the last call, and its highest
	// peak and total samples over full scale for the whole session; nSynth indexes the latter
	constexpr unsigned MaxSynths = 4;
	void LogAudioLevel(unsigned nSynth, const char* pSynthName);

	// Create/truncate the log file; call once the SD card is mounted
	bool OpenLogFile();

	// Append buffered log text to the file (if open); call from the main task (core 0)
	// only, as it is the only user of the file system after startup
	void FlushLogFile();
}

// Logger target that buffers the log for the log file and the LCD, and passes it on
// to the configured log device (if any)
class CDiagLogDevice : public CDevice
{
public:
	CDiagLogDevice() : m_pForwardDevice(nullptr) {}

	void SetForwardDevice(CDevice* pDevice) { m_pForwardDevice = pDevice; }
	virtual int Write(const void* pBuffer, size_t nCount) override;

private:
	CDevice* m_pForwardDevice;
};

#define DIAG_INC(COUNTER)    (++Diag::COUNTER)
#define DIAG_ADD(COUNTER, N) (Diag::COUNTER += (N))

#endif
