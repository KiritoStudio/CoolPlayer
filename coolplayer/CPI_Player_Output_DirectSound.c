/*
 * RTPlayer - Blazing fast audio player.
 * Copyright (C) 2000-2001 Niek Albers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
////////////////////////////////////////////////////////////////////////////////



#include "stdafx.h"
#include "globals.h"
#include "CPI_Player.h"
#include "CPI_Player_CoDec.h"
#include "CPI_Player_Output.h"
#include "CPI_Equaliser.h"
#define DIRECTSOUND_VERSION 0x0500  /* Version 5.0 */
#include <dsound.h>
#include <math.h>

/* WAVEFORMATEXTENSIBLE/WAVE_FORMAT_EXTENSIBLE normally come from mmreg.h via
 * ksmedia.h; declared by hand here rather than pulling in extra WDK headers
 * that may not be present in every cross toolchain. Only defined if the
 * headers we do have haven't already brought them in. */
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif
#ifndef _WAVEFORMATEXTENSIBLE_
#define _WAVEFORMATEXTENSIBLE_
typedef struct {
	WAVEFORMATEX Format;
	union {
		WORD wValidBitsPerSample;
		WORD wSamplesPerBlock;
		WORD wReserved;
	} Samples;
	DWORD dwChannelMask;
	GUID SubFormat;
} WAVEFORMATEXTENSIBLE;
#endif

/* {00000001-0000-0010-8000-00AA00389B71} - fixed, well-known value, same as
 * KSDATAFORMAT_SUBTYPE_PCM */
static const GUID CPC_SUBFORMAT_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT      0x1
#endif
#ifndef SPEAKER_FRONT_RIGHT
#define SPEAKER_FRONT_RIGHT     0x2
#endif
#ifndef SPEAKER_FRONT_CENTER
#define SPEAKER_FRONT_CENTER    0x4
#endif

////////////////////////////////////////////////////////////////////////////////
//
// This is an output stage that uses DirectSound.
// here - just a circular buffer of wave blocks and an event controlling the
// lot.  This should be considered the "reference" output stage.
//
////////////////////////////////////////////////////////////////////////////////

// See the matching comment on CIC_CIRCLE_DATABUFFER_SIZE in
// CPI_Player_CoDec_WinAmpPlugin.c - the old 64KB (~0.37s) DirectSound buffer
// left very little slack against transient stalls upstream (in particular
// the FLAC winamp-plugin bridge's own circle buffer running dry), so bumped
// this up for extra margin too; cheap on memory, only affects latency on
// pause/seek by a fraction of a second.
#define CPC_OUTPUTBLOCKSIZE   0x40000 // 256KB block
#define CPC_MAXFILLAMOUNT   (CPC_OUTPUTBLOCKSIZE>>4) // 1/16th of total block size
#define CPC_INVALIDCURSORPOS  0xFFFFFFFF
////////////////////////////////////////////////////////////////////////////////
//

typedef struct __CPs_OutputContext_DirectSound
{
	LPDIRECTSOUNDBUFFER lpDSB;
	LPDIRECTSOUND lpDirectSound;
	WAVEFORMATEXTENSIBLE WaveFile;
	DWORD m_WriteCursor;
	BOOL m_TermState_Wrapped;
	DWORD m_TermState_WriteCursor;
	DWORD m_TermState_HighestPlayPos;
	DWORD m_TimerId;
	BOOL m_bStreamRunning;
	// Explicit count of bytes written to the DS ring but not yet reached by the
	// hardware play cursor.  Kept as a counter (written bytes in, play-cursor
	// advance out) rather than derived from cursor positions because a full
	// ring and an empty ring both have m_WriteCursor == play cursor.
	DWORD m_dwQueuedBytes;
	DWORD m_dwLastPlayPos;
	CPs_EqualiserModule* m_pEqualiser;
	BYTE* m_pShadowBuffer;
} CPs_OutputContext_DirectSound;

//
////////////////////////////////////////////////////////////////////////////////



void CPP_OMDS_Initialise(CPs_OutputModule* pModule, const CPs_FileInfo* pFileInfo, CP_HEQUALISER hEqualiser);
void CPP_OMDS_Uninitialise(CPs_OutputModule* pModule);
void CPP_OMDS_RefillBuffers(CPs_OutputModule* pModule);
void CPP_OMDS_SetPause(CPs_OutputModule* pModule, const BOOL bPause);
BOOL CPP_OMDS_IsOutputComplete(CPs_OutputModule* pModule);
void CPP_OMDS_Flush(CPs_OutputModule* pModule);
void CPP_OMDS_SetVolume(CPs_OutputModule* pModule, int iVolume);
void CPP_OMDS_GetVolume(CPs_OutputModule* pModule, int *iVolume, HANDLE waitevent);
void CPP_OMDS_EnablePlay(CPs_OutputModule* pModule, const BOOL bEnable);
void CPP_OMDS_OnEQChanged(CPs_OutputModule* pModule);
void CPP_OMDS_SetInternalVolume(CPs_OutputModule* pModule, const int iNewVolume);
int CPP_OMDS_GetOutputLag_ms(CPs_OutputModule* pModule);

////////////////////////////////////////////////////////////////////////////////
//
//
//
void CPI_Player_Output_Initialise_DirectSound(CPs_OutputModule* pModule)
{
	// This is a one off call to set up the function pointers
	pModule->Initialise = CPP_OMDS_Initialise;
	pModule->Uninitialise = CPP_OMDS_Uninitialise;
	pModule->RefillBuffers = CPP_OMDS_RefillBuffers;
	pModule->SetPause = CPP_OMDS_SetPause;
	pModule->IsOutputComplete = CPP_OMDS_IsOutputComplete;
	pModule->Flush = CPP_OMDS_Flush;
	pModule->OnEQChanged = CPP_OMDS_OnEQChanged;
	pModule->SetInternalVolume = CPP_OMDS_SetInternalVolume;
	pModule->GetOutputLag_ms = CPP_OMDS_GetOutputLag_ms;
	pModule->m_pModuleCookie = NULL;
	pModule->m_pcModuleName = "DirectSound Plugout";
	pModule->m_pCoDec = NULL;
	pModule->m_pEqualiser = NULL;
}

//
//
void CPP_OMDS_Initialise(CPs_OutputModule* pModule, const CPs_FileInfo* pFileInfo, CP_HEQUALISER hEqualiser)
{
	// This is called when some playing is required.
	// Do all allocation here so that we do not hold
	// resources while we are just sitting waiting for
	// something to happen.
	
	// Create a context
	DSBUFFERDESC dsbd;
	HRESULT hrRetVal;
	
	CPs_OutputContext_DirectSound* pContext;
	CP_ASSERT(pModule->m_pModuleCookie == NULL);
	pContext = (CPs_OutputContext_DirectSound*)malloc(sizeof(CPs_OutputContext_DirectSound));
	pModule->m_pModuleCookie = pContext;
	CP_TRACE0("DirectSound initialising");
	
	// Create sync object
	pModule->m_evtBlockFree = CreateEvent(NULL, FALSE, FALSE, NULL);
	
	// Create the Direct Sound Object
	
	if (DirectSoundCreate(NULL, &(pContext->lpDirectSound), NULL))
	{
		// On failure lpDirectSound is left invalid; without returning here,
		// the SetCooperativeLevel call below would dereference it.
		CP_FAIL("Cannot create DirectSound Object");
		return;
	}
	
	if (IDirectSound_SetCooperativeLevel(pContext->lpDirectSound, windows.wnd_main, DSSCL_NORMAL))
	{
		// See the matching comment in CPP_OMDS_RefillBuffers: CP_FAIL() does
		// not actually stop execution in release builds, so this must return
		// explicitly or the code below dereferences pContext after
		// CPP_OMDS_Uninitialise() has freed it.
		CPP_OMDS_Uninitialise(pModule);
		CP_FAIL("Can\'t set DirectSound Cooperative level");
		return;
	}

	if (!pContext->lpDirectSound)
	{
		CPP_OMDS_Uninitialise(pModule);
		CP_FAIL("Unable to initialise DirectSound");
		return;
	}
	
	memset(&pContext->WaveFile, 0, sizeof(pContext->WaveFile));
	pContext->WaveFile.Format.nChannels = pFileInfo->m_bStereo ? 2 : 1;
	pContext->WaveFile.Format.nSamplesPerSec = pFileInfo->m_iFreq_Hz;
	pContext->WaveFile.Format.wBitsPerSample = (WORD)pFileInfo->m_iBitsPerSample;
	pContext->WaveFile.Format.nBlockAlign = (pContext->WaveFile.Format.nChannels * pContext->WaveFile.Format.wBitsPerSample) >> 3;
	pContext->WaveFile.Format.nAvgBytesPerSec = pContext->WaveFile.Format.nSamplesPerSec * pContext->WaveFile.Format.nBlockAlign;

	if (pFileInfo->m_iBitsPerSample > 16)
	{
		/* 24-in-32 WAVEFORMATEXTENSIBLE - some driver stacks (this device's
		 * included) don't handle legacy packed >16-bit WAVEFORMATEX cleanly,
		 * producing audible noise; the extensible form is the broadly-supported
		 * way to advertise a >16-bit stream. Sample data is expected to be
		 * left-justified 32-bit little-endian containers (see
		 * FLAC__plugin_common__pack_pcm_signed_left_justified_32_little_endian
		 * on the producer side). */
		pContext->WaveFile.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		pContext->WaveFile.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		pContext->WaveFile.Samples.wValidBitsPerSample = 24;
		pContext->WaveFile.dwChannelMask = pContext->WaveFile.Format.nChannels == 2
			? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
		pContext->WaveFile.SubFormat = CPC_SUBFORMAT_PCM;
	}
	else
	{
		pContext->WaveFile.Format.wFormatTag = WAVE_FORMAT_PCM;
		pContext->WaveFile.Format.cbSize = 0;
	}

	// Create sound buffer
	memset(&dsbd, 0, sizeof(DSBUFFERDESC));
	dsbd.dwSize = sizeof(DSBUFFERDESC);
	dsbd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
	dsbd.dwBufferBytes = CPC_OUTPUTBLOCKSIZE;
	dsbd.lpwfxFormat = &pContext->WaveFile.Format;
	hrRetVal = IDirectSound_CreateSoundBuffer(pContext->lpDirectSound,
			   &dsbd,
			   &(pContext->lpDSB),
			   NULL);
	           
	if (FAILED(hrRetVal))
	{
		// Same hazard: without an explicit return, the code below would call
		// IDirectSoundBuffer_Lock() on the NULL lpDSB just set here.
		pContext->lpDSB = NULL;
		CP_FAIL("Cannot create soundbuffer");
		return;
	}
	
	// Empty sound buffer
	{
		BYTE *pbData = NULL;
		DWORD dwLength;
		
		// Lock DirectSound buffer
		IDirectSoundBuffer_Lock(pContext->lpDSB,
								0,
								CPC_OUTPUTBLOCKSIZE,
								(LPVOID *) &pbData,
								&dwLength,
								NULL,
								NULL,
								0);
		                        
		if (pbData)
			memset(pbData, 0, dwLength);
			
		// Unlock the buffer
		IDirectSoundBuffer_Unlock(pContext->lpDSB,
								  pbData,
								  dwLength,
								  NULL,
								  0L);
	}
	
	// Set bufferoffset to 0
	pContext->m_WriteCursor = CPC_INVALIDCURSORPOS;
	pContext->m_TermState_Wrapped = FALSE;
	pContext->m_TermState_WriteCursor = CPC_INVALIDCURSORPOS;
	pContext->m_TermState_HighestPlayPos = CPC_INVALIDCURSORPOS;
	pContext->m_TimerId = 0;
	pContext->m_bStreamRunning = FALSE;
	pContext->m_dwQueuedBytes = 0;
	pContext->m_dwLastPlayPos = 0;

	// Create shadow buffer (for live EQ)
	pContext->m_pShadowBuffer = (BYTE*)malloc(CPC_OUTPUTBLOCKSIZE);
	
	memset(pContext->m_pShadowBuffer, 0, CPC_OUTPUTBLOCKSIZE);
	
	// Setup thread prioity to max - If you are having problems during
	// debugging (with a big hang!) then comment this next line out
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	
	pModule->m_pEqualiser = hEqualiser;
}

//
//
//
void CPP_OMDS_Uninitialise(CPs_OutputModule* pModule)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);

	// A prior RefillBuffers()/IsOutputComplete() call may already have torn
	// this down (fatal Lock/Unlock failure) and cleared the cookie; the
	// engine loop's own bookkeeping (m_bOutputActive) doesn't know that
	// happened, so Uninitialise() can legitimately be asked to run twice.
	if (!pContext)
		return;

	CP_TRACE0("DirectSound shutting down");
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	
	if (pContext->lpDSB)
	{
		// Destroy the direct sound buffer.
		IDirectSoundBuffer_Release(pContext->lpDSB);
	}
	
	if (pContext->lpDirectSound)
	{
		// Destroy the direct sound object.
		IDirectSound_Release(pContext->lpDirectSound);

		// Must null this out, not just close it - the engine thread's message
		// loop checks "if (m_evtBlockFree)" to decide whether to wait on it
		// (CPI_Player_Engine.c). A closed-but-non-NULL handle passes that
		// check, and MsgWaitForMultipleObjects on an invalid handle returns
		// immediately instead of blocking, turning that wait into a
		// non-yielding spin loop (100% CPU, playback frozen) rather than a
		// clean signal to reinitialise before the next track.
		CloseHandle(pModule->m_evtBlockFree);
		pModule->m_evtBlockFree = NULL;
	}
	
	if (pContext->m_TimerId)
	{
		timeKillEvent(pContext->m_TimerId);
		pContext->m_TimerId = 0;
	}
	
	pContext->lpDirectSound = NULL;
	
	pContext->lpDSB = NULL;
	free(pContext->m_pShadowBuffer);
	free(pContext);
	pContext = NULL;
	pModule->m_pModuleCookie = NULL;
}

//
//
//
void GetPlayPosAndInvalidLength(CPs_OutputContext_DirectSound* pContext, DWORD* pdwPlayPos, DWORD* pdwLength)
{
	HRESULT hrResult;
	
	hrResult = IDirectSoundBuffer_GetCurrentPosition(pContext->lpDSB, pdwPlayPos, NULL);
	
	if (FAILED(hrResult))
		CP_TRACE0("Failed call to IDirectSoundBuffer_GetCurrentPosition");
		
	// Get amount of free data in the buffer
	if (pContext->m_WriteCursor != CPC_INVALIDCURSORPOS)
	{
		if (pContext->m_WriteCursor >= *pdwPlayPos)
			*pdwLength = (CPC_OUTPUTBLOCKSIZE - pContext->m_WriteCursor) + *pdwPlayPos;
		else
			*pdwLength = *pdwPlayPos - pContext->m_WriteCursor;
	}
	
	else
	{
		// First write
		pContext->m_WriteCursor = 0;
		*pdwLength = CPC_OUTPUTBLOCKSIZE;
	}
}

//
//
//
void CPP_OMDS_RefillBuffers(CPs_OutputModule* pModule)
{
	HRESULT hrRetVal;
	BYTE *pbData;
	DWORD dwLength = 0;
	BOOL bMoreData = TRUE;
	DWORD RealLength;
	DWORD dwAmountToFill, dwCurrentPlayCursor;
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);

	// A previous call may have hit a fatal Lock/Unlock failure and torn the
	// module down (see below); the engine loop doesn't know that happened
	// and will keep calling RefillBuffers() on every tick regardless.
	if (!pContext || !pContext->lpDirectSound)
		return;

	GetPlayPosAndInvalidLength(pContext, &dwCurrentPlayCursor, &dwAmountToFill);

	// Retire whatever the play cursor has consumed since we last looked
	{
		DWORD dwAdvance;

		if (dwCurrentPlayCursor >= pContext->m_dwLastPlayPos)
			dwAdvance = dwCurrentPlayCursor - pContext->m_dwLastPlayPos;
		else
			dwAdvance = CPC_OUTPUTBLOCKSIZE - (pContext->m_dwLastPlayPos - dwCurrentPlayCursor);

		pContext->m_dwLastPlayPos = dwCurrentPlayCursor;

		if (pContext->m_dwQueuedBytes > dwAdvance)
			pContext->m_dwQueuedBytes -= dwAdvance;
		else
			pContext->m_dwQueuedBytes = 0;
	}

	// The cursor-derived free length cannot tell a completely full ring from a
	// completely empty one (both have write cursor == play cursor), so when the
	// ring got filled right up to the play cursor and the play cursor has not
	// moved by the next tick, it would be seen as empty and a whole buffer of
	// data would be pulled from the codec again, overwriting audio that has not
	// played yet (an audible skip of a whole buffer's worth - this is what made
	// FLAC tracks "start" several seconds in).  Clamp with the explicit queued
	// count so we only ever fill genuinely free space.
	if (dwAmountToFill > CPC_OUTPUTBLOCKSIZE - pContext->m_dwQueuedBytes)
		dwAmountToFill = CPC_OUTPUTBLOCKSIZE - pContext->m_dwQueuedBytes;

	// Limit the amount of filling that we do per batch
	// - This will improve seek time and skipping on heavilly loaded systems
	if (dwAmountToFill > CPC_MAXFILLAMOUNT && pContext->m_bStreamRunning == FALSE)
		dwAmountToFill = CPC_MAXFILLAMOUNT;
		
	// Try to totally fill the buffer with sound data - this is the best policy because our
	// timer (event trigger) is likely to have v.poor accrucy
	if (dwAmountToFill > 0)
	{
		// Lock DirectSound buffer
		hrRetVal = IDirectSoundBuffer_Lock(pContext->lpDSB,
										   0,
										   CPC_OUTPUTBLOCKSIZE,
										   (LPVOID *) & pbData,
										   &dwLength,
										   NULL,
										   NULL,
										   0);
		                                   
		if (FAILED(hrRetVal))
		{
			// CP_FAIL() is a no-op in release builds (it only breaks into a
			// debugger under _DEBUG), so without an explicit return here
			// execution fell through into code that dereferences pContext
			// (just freed by CPP_OMDS_Uninitialise) and writes through pbData
			// (never set - Lock failed), a guaranteed use-after-free / wild
			// write. This was the actual cause of the intermittent
			// DSound-refill crashes.
			CPP_OMDS_Uninitialise(pModule);
			CP_FAIL("Cannot lock soundbuffer");

			// Tell the engine loop this stream is dead too, otherwise it
			// keeps thinking output is healthy (m_pCoDec still set) and
			// calls straight back into this now-torn-down module next tick
			// (UpdateProgress -> GetOutputLag_ms etc.) - same crash, one
			// call later. Closing the codec here makes the engine's
			// existing "ran out of data" bookkeeping take over cleanly.
			if (pModule->m_pCoDec)
			{
				pModule->m_pCoDec->CloseFile(pModule->m_pCoDec);
				pModule->m_pCoDec = NULL;
			}
			return;
		}

		// Write data into shadow buffer
		
		if ((pContext->m_WriteCursor + dwAmountToFill) >= CPC_OUTPUTBLOCKSIZE)
			RealLength = CPC_OUTPUTBLOCKSIZE - pContext->m_WriteCursor;
		else
			RealLength = dwAmountToFill;
			
		if (RealLength)
		{
			bMoreData = pModule->m_pCoDec->GetPCMBlock(pModule->m_pCoDec, pContext->m_pShadowBuffer + pContext->m_WriteCursor, &RealLength);
			memcpy(pbData + pContext->m_WriteCursor, pContext->m_pShadowBuffer + pContext->m_WriteCursor, RealLength);
		}
		
		// If there is EQ then apply it
		// (the EQ implementation is hardwired to packed 16-bit stereo samples,
		// see CIC_DECODESAMPLE_LEFT/RIGHT in CPI_Equaliser_Basic.c - running it
		// over 8-bit or 24-bit data would corrupt the audio, so just skip it there)
		{
			// Note that the EQ module is initialised and uninitialsed by the engine
			CPs_EqualiserModule* pEQModule = (CPs_EqualiserModule*)pModule->m_pEqualiser;

			if (RealLength && pContext->WaveFile.Format.wBitsPerSample == 16)
				pEQModule->ApplyEQToBlock_Inplace(pEQModule, pbData + pContext->m_WriteCursor, RealLength);
		}
		
		// Move cursor
		pContext->m_WriteCursor += RealLength;

		// Account for the freshly queued (not yet audible) data
		pContext->m_dwQueuedBytes += RealLength;

		if (pContext->m_dwQueuedBytes > CPC_OUTPUTBLOCKSIZE)
			pContext->m_dwQueuedBytes = CPC_OUTPUTBLOCKSIZE;

		if (pContext->m_WriteCursor >= CPC_OUTPUTBLOCKSIZE)
			pContext->m_WriteCursor -= CPC_OUTPUTBLOCKSIZE;
			
		// Unlock the buffer
		hrRetVal = IDirectSoundBuffer_Unlock(pContext->lpDSB,
											 pbData,
											 dwLength,
											 NULL,
											 0L);
		                                     
		if (FAILED(hrRetVal))
		{
			// Same use-after-free hazard as the Lock failure path above.
			CPP_OMDS_Uninitialise(pModule);
			CP_FAIL("Cannot Unlock soundbuffer");

			if (pModule->m_pCoDec)
			{
				pModule->m_pCoDec->CloseFile(pModule->m_pCoDec);
				pModule->m_pCoDec = NULL;
			}
			return;
		}
	}
	
	if (bMoreData == FALSE)
	{
		CP_TRACE0("Stream exhausted");
		pModule->m_pCoDec->CloseFile(pModule->m_pCoDec);
		pModule->m_pCoDec = NULL;
	}
	
	pContext->m_TermState_Wrapped = FALSE;
	
	pContext->m_TermState_WriteCursor = CPC_INVALIDCURSORPOS;
	pContext->m_TermState_HighestPlayPos = CPC_INVALIDCURSORPOS;
	
	if (!pContext->m_bStreamRunning)
		CPP_OMDS_EnablePlay(pModule, TRUE);
}

//
//
//
void CPP_OMDS_SetPause(CPs_OutputModule* pModule, const BOOL bPause)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);

	if (!pContext || !pContext->lpDirectSound)
		return;
		
	CPP_OMDS_EnablePlay(pModule, !bPause);
}

//
//
//
BOOL CPP_OMDS_IsOutputComplete(CPs_OutputModule* pModule)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	DWORD dwCurrentPlayCursor, dwInvalidLength;
	CP_CHECKOBJECT(pContext);

	// Sound isn't open (or was torn down by a fatal RefillBuffers failure)

	if (!pContext || !pContext->lpDirectSound)
		return TRUE;
		
	// Was there ever any data in this buffer
	if (pContext->m_WriteCursor == CPC_INVALIDCURSORPOS)
		return TRUE;
		
	// Work out write cursor at terminate time
	if (pContext->m_TermState_WriteCursor == CPC_INVALIDCURSORPOS)
		pContext->m_TermState_WriteCursor = pContext->m_WriteCursor;
		
	if (pModule->m_pCoDec)
		return FALSE;
		
	GetPlayPosAndInvalidLength(pContext, &dwCurrentPlayCursor, &dwInvalidLength);
	
	// Empty the invalid area - this is because we cannot be sure that some of the
	// invalid area will no be played because our event doesn't com in on time
	if (dwInvalidLength > 0)
	{
		BYTE *pbData = NULL;
		DWORD dwLength = 0;
		
		// Lock DirectSound buffer
		IDirectSoundBuffer_Lock(pContext->lpDSB,
								pContext->m_WriteCursor,
								dwInvalidLength,
								(LPVOID *) &pbData,
								&dwLength,
								NULL,
								NULL,
								0);
		                        
		if (pbData)
			memset(pbData, 0, dwLength);
			
		// Unlock the buffer
		IDirectSoundBuffer_Unlock(pContext->lpDSB,
								  pbData,
								  dwLength,
								  NULL,
								  0L);
		                          
		// Move write cursor
		pContext->m_WriteCursor += dwInvalidLength;
		
		if (pContext->m_WriteCursor >= CPC_OUTPUTBLOCKSIZE)
			pContext->m_WriteCursor -= CPC_OUTPUTBLOCKSIZE;
			
	}
	
	// For this function to work there needs to be 2 calls - one to detect the wrap
	// and another to detect the termination - it is therefore necessary for the
	// event to be triggered on a reasonably regular interval
	
	if (pContext->m_TermState_HighestPlayPos == CPC_INVALIDCURSORPOS || pContext->m_TermState_HighestPlayPos < dwCurrentPlayCursor)
		pContext->m_TermState_HighestPlayPos = dwCurrentPlayCursor;
	else
		pContext->m_TermState_Wrapped = TRUE;
		
	if (pContext->m_TermState_Wrapped == FALSE)
		return FALSE;
		
	if (dwCurrentPlayCursor > pContext->m_TermState_WriteCursor)
		return TRUE;
		
	return FALSE;
}

//
//
//
void CPP_OMDS_Flush(CPs_OutputModule* pModule)
{
	DWORD dwCurrentPlayCursor;
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);

	if (!pContext || !pContext->lpDirectSound)
		return;

	CPP_OMDS_EnablePlay(pModule, FALSE);
	
	IDirectSoundBuffer_GetCurrentPosition(pContext->lpDSB, &dwCurrentPlayCursor, NULL);

	pContext->m_WriteCursor = dwCurrentPlayCursor;
	pContext->m_dwQueuedBytes = 0;
	pContext->m_dwLastPlayPos = dwCurrentPlayCursor;
}

//
//
//
void CPP_OMDS_EnablePlay(CPs_OutputModule* pModule, const BOOL bEnable)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);

	if (!pContext)
		return;

	pContext->m_bStreamRunning = bEnable;
	
	// Setup event timer - according to our current requirements
	
	if (bEnable)
	{
		IDirectSoundBuffer_Play(pContext->lpDSB, 0, 0, DSBPLAY_LOOPING);
		
		// If there is a CoDec set up looping play mode
		
		if (pContext->m_TimerId == 0)
		{
			pContext->m_TimerId = timeSetEvent(20,
											   10, // 10ms Resolution
											   (LPTIMECALLBACK)pModule->m_evtBlockFree,
											   0,
											   TIME_PERIODIC | TIME_CALLBACK_EVENT_SET);
		}
	}
	
	else
	{
		IDirectSoundBuffer_Stop(pContext->lpDSB);
		timeKillEvent(pContext->m_TimerId);
		pContext->m_TimerId = 0;
	}
}

//
//
//
void CPP_OMDS_OnEQChanged(CPs_OutputModule* pModule)
{
#if 1
#ifdef _DEBUG
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	CP_CHECKOBJECT(pContext);
#endif
#else
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	BYTE *pbData = NULL;
	DWORD dwLength = 0;
//    DWORD dwPlayPos;
//    CPs_EqualiserModule* pEQModule;
	CP_CHECKOBJECT(pContext);
	
	// Get the current play pos
	IDirectSoundBuffer_GetCurrentPosition(pContext->lpDSB, &dwPlayPos, NULL);
	
	// Lock DirectSound buffer
	IDirectSoundBuffer_Lock(pContext->lpDSB,
							0,
							CPC_OUTPUTBLOCKSIZE,
							&pbData,
							&dwLength,
							NULL,
							NULL,
							0);
	
	if (pbData)
		memcpy(pbData, pContext->m_pShadowBuffer, dwLength);
	
	// Apply EQ from play to either the end or the write cursor
	// (skipped for non-16-bit streams - see the comment on the other
	// ApplyEQToBlock_Inplace call site in this file)
	pEQModule = (CPs_EqualiserModule*)pModule->m_pEqualiser;

	if (pContext->WaveFile.Format.wBitsPerSample == 16)
	{
		if (pContext->m_WriteCursor > dwPlayPos)
		{
			// One block - from play pos to write cursor
			if (pContext->m_WriteCursor != CPC_INVALIDCURSORPOS)
				pEQModule->ApplyEQToBlock_Inplace(pEQModule, pbData + dwPlayPos, pContext->m_WriteCursor - dwPlayPos);
		}

		else
		{
			// Two blocks - from play pos to end
			// - and from start to write cursor
			pEQModule->ApplyEQToBlock_Inplace(pEQModule, pbData + dwPlayPos, CPC_OUTPUTBLOCKSIZE - dwPlayPos);
			pEQModule->ApplyEQToBlock_Inplace(pEQModule, pbData, pContext->m_WriteCursor);
		}
	}
	
	// Unlock the buffer
	IDirectSoundBuffer_Unlock(pContext->lpDSB,
							  pbData,
							  dwLength,
							  NULL,
							  0L);
	
#endif
}

//
//
//
int CPP_OMDS_GetOutputLag_ms(CPs_OutputModule* pModule)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	DWORD dwPlayPos, dwAdvance;
	HRESULT hrResult;
	CP_CHECKOBJECT(pContext);

	// pContext is NULL here if a fatal Lock/Unlock failure tore the module
	// down mid-tick (see CPP_OMDS_RefillBuffers) - UpdateProgress() calls
	// straight back into this on the very next line of the engine loop, so
	// this NULL check is load-bearing, not defensive filler.
	if (!pContext || !pContext->lpDirectSound || !pContext->lpDSB
			|| pContext->WaveFile.Format.nAvgBytesPerSec == 0)
		return 0;

	hrResult = IDirectSoundBuffer_GetCurrentPosition(pContext->lpDSB, &dwPlayPos, NULL);

	if (FAILED(hrResult))
		return 0;

	// Retire whatever the play cursor has consumed since we last looked
	// (this is called at least once per refill tick, i.e. much more often
	// than the ring wraps, so the modulo distance is unambiguous)
	if (dwPlayPos >= pContext->m_dwLastPlayPos)
		dwAdvance = dwPlayPos - pContext->m_dwLastPlayPos;
	else
		dwAdvance = CPC_OUTPUTBLOCKSIZE - (pContext->m_dwLastPlayPos - dwPlayPos);

	pContext->m_dwLastPlayPos = dwPlayPos;

	if (pContext->m_dwQueuedBytes > dwAdvance)
		pContext->m_dwQueuedBytes -= dwAdvance;
	else
		pContext->m_dwQueuedBytes = 0;

	return (int)((pContext->m_dwQueuedBytes * 1000) / pContext->WaveFile.Format.nAvgBytesPerSec);
}

//
//
//
void CPP_OMDS_SetInternalVolume(CPs_OutputModule* pModule, const int iNewVolume)
{
	CPs_OutputContext_DirectSound* pContext = (CPs_OutputContext_DirectSound*)pModule->m_pModuleCookie;
	LONG lVolume;
	CP_CHECKOBJECT(pContext);

	if (!pContext || !pContext->lpDSB)
		return;

	lVolume = (int)(pow((double)(100 - iNewVolume) * 0.01, 3) * (double)DSBVOLUME_MIN);  //((100-iNewVolume) * DSBVOLUME_MIN) / 500;
	IDirectSoundBuffer_SetVolume(pContext->lpDSB, lVolume);
}

//
//
//

