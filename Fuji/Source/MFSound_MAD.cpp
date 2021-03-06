#include "Fuji_Internal.h"

#if defined(MAD_STREAM)

#include "MFSound_Internal.h"
#include "MFFileSystem.h"
#include "FileSystem/MFFileSystemCachedFile.h"
#include "FileSystem/MFFileSystemMemory.h"
#include "MFHeap.h"

// MAD stream handler
#if defined(MF_WINDOWS)
	#pragma comment(lib, "mad")
#endif

#define SAMPLES_PER_MP3_FRAME 1152

#define INPUT_BUFFER_SIZE	(5*8192)
#define OUTPUT_BUFFER_SIZE	8192 /* Must be an integer multiple of 4. */

#if !defined(MF_LINUX) && !defined(MF_OSX)
	#include <global.h>
#endif
#include <mad.h>

struct MFID3
{
	unsigned char *pID3Data;
	uint32 size;
	uint8 major, minor;
	uint8 flags;
	uint8 reserved;
};

struct MFID3v1
{
	char TAG[3];
	char title[30];
	char artist[30];
	char album[30];
	char year[4];
	char comment[30];
	unsigned char genre;
};

enum ID3v1_Enhanced_Speed
{
	ID3v1_Speed_Unset,
	ID3v1_Speed_Slow,
	ID3v1_Speed_Medium,
	ID3v1_Speed_Fast,
	ID3v1_Speed_Hardcore
};

struct MFID3v1_Enhanced
{
	char TAG[4];
	char title[60];
	char artist[60];
	char album[60];
	unsigned int speed; // From enum ID3v1_Enhanced_Speed
	char genre[30];
	char start_time[6];
	char end_time[6];
};

struct ID3Chunk
{
	uint32 id;
	uint32 size;
	uint16 flags;
};

struct MP3Header
{
	int audioVersion;
	int layer;
	int bitrate;
	int samplerate;
	int channelMode;
	int modeExt;
	int padding;

	int frameSize;

	bool protectionBit;
	bool copyright;
	bool original;
};

struct MFMADDecoder
{
	unsigned char inputBuffer[INPUT_BUFFER_SIZE+MAD_BUFFER_GUARD];
	unsigned char overflow[OUTPUT_BUFFER_SIZE];
	mad_stream stream;
	mad_frame frame;
	mad_synth synth;
	mad_header firstHeader;
	mad_timer_t timer;
	MFFile *pFile;
	MFID3 *pID3;

	uint32 *pFrameOffsets;
	uint32 numFrameOffsetsAllocated;
	uint32 frameOffsetCount;

	unsigned char *pGuardPtr;
	uint32 currentFrame;
	uint32 frameCount;
	size_t overflowOffset;
	size_t overflowBytes;
};

int GetSynchSafeInt(unsigned char *pStream)
{
	return (uint32)pStream[3] | ((uint32)pStream[2] << 7) | ((uint32)pStream[1] << 14) | ((uint32)pStream[0] << 21);
}

static inline signed short MadFixedToSshort(mad_fixed_t fixed)
{
	// clipping
	if(fixed >= MAD_F_ONE)
		return 32767;
	if(fixed <= -MAD_F_ONE)
		return -32767;

	// conversion
	fixed = fixed >> (MAD_F_FRACBITS - 15);
	return (signed short)fixed;
}

void ParseID3(MFAudioStream *pStream, MFID3 *pID3, size_t dataSize)
{
	unsigned char *pData = pID3->pID3Data;

	while(pData - pID3->pID3Data <= (ptrdiff_t)dataSize)
	{
		if(pID3->major == 2)
		{
			// for the moment, to be safe...
//			MFDebug_Assert(false, "Make this work!!");
			return;

			//int size = (int)pData[5] | ((int)pData[4] << 7) | ((int)pData[3] << 14);
			//pData += 6 + size;
		}
		if(pID3->major > 2)
		{
			ID3Chunk *pChunkHeader = (ID3Chunk*)pData;
			int size = GetSynchSafeInt(pData + 4);

			if(size == 0)
				break;

			if(*pData == 'T')
			{
				//uint8 encoding = pData[10];
				const char *pString = (const char*)pData + 11;

				switch(pChunkHeader->id)
				{
					case MFMAKEFOURCC('T', 'I', 'T', '2'):
						MFString_CopyN(pStream->streamInfo.songName, pString, MFMin(size-1, (int)sizeof(pStream->streamInfo.songName)-1));
						break;
					case MFMAKEFOURCC('T', 'A', 'L', 'B'):
						MFString_CopyN(pStream->streamInfo.albumName, pString, MFMin(size-1, (int)sizeof(pStream->streamInfo.albumName)-1));
						break;
					case MFMAKEFOURCC('T', 'P', 'E', '1'):
					case MFMAKEFOURCC('T', 'P', 'E', '2'):
						MFString_CopyN(pStream->streamInfo.artistName, pString, MFMin(size-1, (int)sizeof(pStream->streamInfo.artistName)-1));
						break;
					case MFMAKEFOURCC('T', 'C', 'O', 'N'):
						MFString_CopyN(pStream->streamInfo.genre, pString, MFMin(size-1, (int)sizeof(pStream->streamInfo.genre)-1));
						break;
				}
			}

			pData += 10 + size;
		}
	}
}

static int bitrates[] =
{
	// Version 1
	0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0,	// Layer 1
	0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0,		// Layer 2
	0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0,		// Layer 3
	// Version 2
	0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0,		// Layer 1
	0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,			// Layer 2
	0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,			// Layer 3
	// Version 2.5
	0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0,		// Layer 1
	0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0,			// Layer 2
	0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0				// Layer 3
};

static int sampleRateTable[] =
{
	44100, 48000, 32000, 0,
	22050, 24000, 16000, 0,
	11025, 12000, 8000, 0,
	0, 0, 0, 0
};

static int versionTable[] = { 2, -1, 1, 0 };
static int layerTable[] = { -1, 2, 1, 0 };

int ReadMP3Header(MP3Header *pHeader, const unsigned char *pHeaderBuffer)
{
	// header is big endian
	uint32 header = pHeaderBuffer[0];
	header = (header<<8) | pHeaderBuffer[1];
	header = (header<<8) | pHeaderBuffer[2];
	header = (header<<8) | pHeaderBuffer[3];

	// check that we are looking at a frame header
	if((header & 0xFFE00000) != 0xFFE00000)
		return 1;

	// extract details from frame header
	pHeader->audioVersion = (header & 0x180000) >> 19;
	pHeader->layer = (header & 0x60000) >> 17;
	pHeader->protectionBit = !!(header & 0x10000);
	pHeader->padding = (header & 0x200) >> 9;

	int bitrate = (header & 0xF000) >> 12;
	int version = versionTable[pHeader->audioVersion];
	int version_layer = (version * 3) + layerTable[pHeader->layer];
	if(bitrate == 0 || bitrate == 0xF || version_layer < 0)
		return 2;
	pHeader->bitrate = bitrates[(version_layer << 4) | bitrate];

	int sampleRate = (header & 0xC00) >> 10;
	pHeader->samplerate = sampleRateTable[(version << 2) | sampleRate];

	// need to read the rest of this shit at some point..
//	pHeader->channelMode
//	pHeader->copyright
//	pHeader->modeExt
//	pHeader->original

	// calculate frame size
	pHeader->frameSize = 144000*pHeader->bitrate/pHeader->samplerate + pHeader->padding;
	return 0;
}

int ScanForFrames(MFAudioStream *pStream, MFMADDecoder *pDecoder, bool bOnlyFirst)
{
	MP3Header header;
	unsigned char buffer[256];
	int sampleRate = 0;

	uint64 filePos = MFFile_Tell(pDecoder->pFile);
	size_t bytes = MFFile_Read(pDecoder->pFile, buffer, bOnlyFirst ? 4 : sizeof(buffer));
	size_t offset = 0;

	while(1)
	{
		size_t remaining = bytes - offset;
		if(remaining < 4)
		{
			filePos += offset;
			if(offset > bytes)
			{
				MFFile_Seek(pDecoder->pFile, filePos, MFSeek_Begin);
				offset = 0;
			}
			else
			{
				int i = 0;
				for(; offset < bytes; ++i, ++offset)
					buffer[i] = buffer[offset];
				offset = i;
			}

			// read more data...
			remaining = bytes = MFFile_Read(pDecoder->pFile, buffer + offset, sizeof(buffer) - offset) + offset;
			offset = 0;

			if(bytes < 4)
				break; // not enough data!
		}

		bool bValidHeader = buffer[offset] == 0xFF && (buffer[offset+1] & 0xE0) == 0xE0 && ReadMP3Header(&header, buffer + offset) == 0;

		if(bValidHeader)
		{
			// should we stash the decoded header somewhere?

			sampleRate = header.samplerate;

			if(pDecoder->frameCount >= pDecoder->numFrameOffsetsAllocated)
			{
				pDecoder->numFrameOffsetsAllocated *= 2;
				pDecoder->pFrameOffsets = (uint32*)MFHeap_Realloc(pDecoder->pFrameOffsets, sizeof(uint32) * pDecoder->numFrameOffsetsAllocated);
			}
			pDecoder->pFrameOffsets[pDecoder->frameOffsetCount++] = (uint32)(filePos + offset);
			++pDecoder->frameCount;

			if(bOnlyFirst)
				break;

			offset += header.frameSize;
		}
		else if(buffer[offset] == 'T' && buffer[offset+1] == 'A' && buffer[offset+2] == 'G')
		{
			if(buffer[offset+3] == '+')
			{
				MFID3v1_Enhanced ID3v1;
				MFCopyMemory(&ID3v1, buffer + offset, MFMin(remaining, sizeof(MFID3v1_Enhanced)));
				if(remaining < 128)
					MFFile_Read(pDecoder->pFile, (char*)&ID3v1 + remaining, sizeof(MFID3v1_Enhanced) - remaining, false);
				offset += sizeof(MFID3v1_Enhanced);

				if(pStream->streamInfo.artistName[0] == 0)
					MFString_CopyN(pStream->streamInfo.artistName, ID3v1.artist, sizeof(ID3v1.artist));
				if(pStream->streamInfo.albumName[0] == 0)
					MFString_CopyN(pStream->streamInfo.albumName, ID3v1.album, sizeof(ID3v1.album));
				if(pStream->streamInfo.songName[0] == 0)
					MFString_CopyN(pStream->streamInfo.songName, ID3v1.title, sizeof(ID3v1.title));
				if(pStream->streamInfo.genre[0] == 0)
					MFString_CopyN(pStream->streamInfo.songName, ID3v1.genre, sizeof(ID3v1.genre));
				// TODO: something with speed/start_time/end_time?
			}
			else
			{
				MFID3v1 ID3v1;
				MFCopyMemory(&ID3v1, buffer + offset, MFMin(remaining, sizeof(MFID3v1)));
				if(remaining < 128)
					MFFile_Read(pDecoder->pFile, (char*)&ID3v1 + remaining, sizeof(MFID3v1) - remaining, false);
				offset += sizeof(MFID3v1);

				if(pStream->streamInfo.artistName[0] == 0)
					MFString_CopyN(pStream->streamInfo.artistName, ID3v1.artist, sizeof(ID3v1.artist));
				if(pStream->streamInfo.albumName[0] == 0)
					MFString_CopyN(pStream->streamInfo.albumName, ID3v1.album, sizeof(ID3v1.album));
				if(pStream->streamInfo.songName[0] == 0)
					MFString_CopyN(pStream->streamInfo.songName, ID3v1.title, sizeof(ID3v1.title));
				if(pStream->streamInfo.trackNumber == 0)
					pStream->streamInfo.trackNumber = ID3v1.comment[28] == 0 ? ID3v1.comment[29] : 0;
				if(pStream->streamInfo.year == 0)
				{
					// TODO: parse 4 bytes into numbers
				}
				if(pStream->streamInfo.genre[0] == 0)
				{
					// TODO: genre is looked up from a big table of possible genres...
				}
			}
		}
		else if(buffer[offset] == 'I' && buffer[offset+1] == 'D' && buffer[offset+2] == '3')
		{
			unsigned char id3Header[10];

			MFCopyMemory(id3Header, buffer + offset, MFMin(remaining, (size_t)10));
			if(remaining < 10)
				MFFile_Read(pDecoder->pFile, id3Header + remaining, 10 - remaining, false);
			offset += 10;

			// ID3 v2...
			size_t size = GetSynchSafeInt(id3Header + 6);

			if(!pDecoder->pID3)
			{
				pDecoder->pID3 = (MFID3*)MFHeap_Alloc(sizeof(MFID3) + size);
				pDecoder->pID3->pID3Data = (unsigned char*)(pDecoder->pID3 + 1);
				pDecoder->pID3->size = (uint32)size;
				pDecoder->pID3->major = buffer[3];
				pDecoder->pID3->minor = buffer[4];
				pDecoder->pID3->flags = buffer[5];

				size_t tagBytes = 0;
				if(offset < bytes)
				{
					tagBytes = MFMin(size, bytes - offset);
					MFCopyMemory(pDecoder->pID3->pID3Data, buffer + offset, tagBytes);
				}
				if(tagBytes < size)
					MFFile_Read(pDecoder->pFile, pDecoder->pID3->pID3Data + tagBytes, size - tagBytes, false);
				offset += size;

				ParseID3(pStream, pDecoder->pID3, size);
			}
		}
		else
			++offset;
	}

	return sampleRate;
}

size_t GetMADSamples(MFAudioStream *pStream, void *pBuffer, size_t bytes)
{
	MFMADDecoder *pDecoder = (MFMADDecoder*)pStream->pStreamData;

	size_t written = 0;

	if(pDecoder->overflowBytes)
	{
		// grab from the overflow until we run out...
		size_t numBytes = MFMin(pDecoder->overflowBytes - pDecoder->overflowOffset, bytes);
		MFCopyMemory(pBuffer, pDecoder->overflow + pDecoder->overflowOffset, numBytes);
		pDecoder->overflowOffset += numBytes;
		if(pDecoder->overflowOffset == pDecoder->overflowBytes)
			pDecoder->overflowBytes = 0;

		// increment timer
		mad_timer_t t = { 0, MAD_TIMER_RESOLUTION / pDecoder->firstHeader.samplerate };
		mad_timer_multiply(&t, (long)(numBytes >> 2) );
		mad_timer_add(&pDecoder->timer, t);

		if(bytes == numBytes)
			return numBytes;

		bytes -= numBytes;
		(char*&)pBuffer += numBytes;
		written = numBytes;
	}

	do
	{
		if(pDecoder->stream.buffer==NULL || pDecoder->stream.error == MAD_ERROR_BUFLEN)
		{
			size_t readSize, remaining;
			unsigned char *readStart;

			if(pDecoder->stream.next_frame != NULL)
			{
				remaining = pDecoder->stream.bufend - pDecoder->stream.next_frame;
				for(size_t a=0; a<remaining; ++a)
					pDecoder->inputBuffer[a] = pDecoder->stream.next_frame[a];
//				memmove(pDecoder->inputBuffer, pDecoder->stream.next_frame, remaining);
				readStart = pDecoder->inputBuffer + remaining;
				readSize = INPUT_BUFFER_SIZE - remaining;
			}
			else
			{
				readSize = INPUT_BUFFER_SIZE;
				readStart = pDecoder->inputBuffer;
				remaining = 0;
			}

			readSize = MFFile_Read(pDecoder->pFile, readStart, readSize, false);
			if(readSize <= 0)
			{
				if(MFFile_IsEOF(pDecoder->pFile))
					MFDebug_Warn(3, "End of input stream.");
				else
					MFDebug_Warn(2, "Read error on bit-stream.");
				break;
			}
			if(MFFile_IsEOF(pDecoder->pFile))
			{
				pDecoder->pGuardPtr = readStart + readSize;
				MFZeroMemory(pDecoder->pGuardPtr, MAD_BUFFER_GUARD);
				readSize += MAD_BUFFER_GUARD;
			}

			mad_stream_buffer(&pDecoder->stream, pDecoder->inputBuffer, (unsigned long)(readSize + remaining));
			pDecoder->stream.error = MAD_ERROR_NONE;
		}

		if(mad_frame_decode(&pDecoder->frame, &pDecoder->stream))
		{
			if(!(pDecoder->stream.error & 0x200))
			{
				if(MAD_RECOVERABLE(pDecoder->stream.error))
				{
					if(pDecoder->stream.error != MAD_ERROR_LOSTSYNC || pDecoder->stream.this_frame != pDecoder->pGuardPtr)
						MFDebug_Warn(3, MFStr("Recoverable frame level error (%s)\n", mad_stream_errorstr(&pDecoder->stream)));
					continue;
				}
				else
				{
					if(pDecoder->stream.error == MAD_ERROR_BUFLEN)
						continue;
					else
					{
						MFDebug_Warn(3, MFStr("Unrecoverable frame level error (%s)\n", mad_stream_errorstr(&pDecoder->stream)));
						break;
					}
				}
			}
			else
			{
				MFDebug_Warn(3, MFStr("Recoverable frame level error (%s)\n", mad_stream_errorstr(&pDecoder->stream)));
			}
		}

		// we'll keep a copy of the first frames header (it has some useful information)...
		if(!pDecoder->firstHeader.samplerate)
			pDecoder->firstHeader = pDecoder->frame.header;

		// we need to keep a backlog of frame offsets for seeking...
		if(pDecoder->currentFrame == pDecoder->frameCount)
		{
			// we'll only take a frame offset once every 10 frames (otherwise we waste loads of memory!)
			// we need to push the counter along from time to time
//			if(pDecoder->frameCount % 10 == 0)
			{
				++pDecoder->frameOffsetCount;

				// if we have overflowed the frame history, we need to realloc...
				if(pDecoder->frameOffsetCount == pDecoder->numFrameOffsetsAllocated)
				{
					pDecoder->numFrameOffsetsAllocated *= 2;
					pDecoder->pFrameOffsets = (uint32*)MFHeap_Realloc(pDecoder->pFrameOffsets, sizeof(uint32)*pDecoder->numFrameOffsetsAllocated);
				}

				pDecoder->pFrameOffsets[pDecoder->frameOffsetCount] = pDecoder->pFrameOffsets[pDecoder->frameOffsetCount - 1];
			}

			// add current frames size to frame offset counter
			int frameSize = (int)(pDecoder->stream.next_frame - pDecoder->stream.this_frame);
			pDecoder->pFrameOffsets[pDecoder->frameOffsetCount] += frameSize;

			// increase counters
			++pDecoder->frameCount;
		}
		++pDecoder->currentFrame;

/*
		// filters?? eq/etc?
		if(DoFilter)
			ApplyFilter(&pDecoder->frame);
*/
		const size_t bytesPerFrame = SAMPLES_PER_MP3_FRAME*4;
		if(pBuffer || bytes < bytesPerFrame)
			mad_synth_frame(&pDecoder->synth, &pDecoder->frame);

		// increment timer
		mad_timer_t t = { 0, MAD_TIMER_RESOLUTION / pDecoder->firstHeader.samplerate };
		mad_timer_multiply(&t, MFMin((long)pDecoder->synth.pcm.length, (long)(bytes >> 2)));
		mad_timer_add(&pDecoder->timer, t);

		bool bStereo = MAD_NCHANNELS(&pDecoder->frame.header) == 2;
		int16 *pOutputPtr = (int16*)pBuffer;

		int i=0;
		for(; i < pDecoder->synth.pcm.length && bytes; i++)
		{
			if(pOutputPtr)
			{
				signed short sample = MadFixedToSshort(pDecoder->synth.pcm.samples[0][i]);
				*(pOutputPtr++) = sample;
				if(bStereo)
					sample = MadFixedToSshort(pDecoder->synth.pcm.samples[1][i]);
				*(pOutputPtr++) = sample;

				written += 4;
			}
			bytes -= 4;
		}
		pBuffer = pOutputPtr;

		// write any remaining samples to the overflow buffer
		pOutputPtr = (int16*)pDecoder->overflow;
		pDecoder->overflowOffset = 0;
		pDecoder->overflowBytes = (pDecoder->synth.pcm.length - i) * 4;

		for(; i < pDecoder->synth.pcm.length; i++)
		{
			signed short sample = MadFixedToSshort(pDecoder->synth.pcm.samples[0][i]);
			*(pOutputPtr++) = sample;
			if(bStereo)
				sample = MadFixedToSshort(pDecoder->synth.pcm.samples[1][i]);
			*(pOutputPtr++) = sample;
		}
	}
	while(bytes);

	return written;
}

void DestroyMADStream(MFAudioStream *pStream)
{
	MFMADDecoder *pDecoder = (MFMADDecoder*)pStream->pStreamData;

	mad_synth_finish(&pDecoder->synth);
	mad_frame_finish(&pDecoder->frame);
	mad_stream_finish(&pDecoder->stream);

	MFFile_Close(pDecoder->pFile);

	if(pDecoder->pID3)
		MFHeap_Free(pDecoder->pID3);
	MFHeap_Free(pDecoder->pFrameOffsets);
	MFHeap_Free(pDecoder);
}

void CreateMADStream(MFAudioStream *pStream, const char *pFilename)
{
	MFCALLSTACK;

	// open mp3 file
	MFFile* hFile = MFFileSystem_Open(pFilename);
	if(!hFile)
		return;

	// if we were granted permission to buffer the whole thing, let's try and do that (speed up the rest of the steps)
	bool cacheFile = true;
	if(pStream->createFlags & MFASF_AllowBuffering)
	{
		uint64 fileSize = MFFile_GetSize(hFile);
		MFDebug_Assert(fileSize > 0, "File is empty, or length is unknown.");

		void *pMP3File = MFHeap_Alloc((size_t)fileSize);
		if(pMP3File)
		{
			MFFile_Read(hFile, pMP3File, (size_t)fileSize);
			MFFile_Close(hFile);

			MFOpenDataMemory memoryFile;
			memoryFile.cbSize = sizeof(MFOpenDataMemory);
			memoryFile.openFlags = MFOF_Read | MFOF_Binary;
			memoryFile.pMemoryPointer = pMP3File;
			memoryFile.fileSize = fileSize;
			memoryFile.allocated = (size_t)fileSize;
			memoryFile.ownsMemory = true;
			hFile = MFFile_Open(MFFileSystem_GetInternalFileSystemHandle(MFFSH_MemoryFileSystem), &memoryFile);
			cacheFile = false;
		}
	}

	if(cacheFile)
	{
		// attempt to cache the mp3 stream (in the case we didn't load it all into memory)
		MFOpenDataCachedFile cachedOpen;
		cachedOpen.cbSize = sizeof(MFOpenDataCachedFile);
		cachedOpen.openFlags = MFOF_Read | MFOF_Binary | MFOF_Cached_CleanupBaseFile;
		cachedOpen.maxCacheSize = 256*1024; // 256k cache for mp3 stream should be heaps!!
		cachedOpen.pBaseFile = hFile;

		MFFile *pCachedFile = MFFile_Open(MFFileSystem_GetInternalFileSystemHandle(MFFSH_CachedFileSystem), &cachedOpen);
		if(pCachedFile)
			hFile = pCachedFile;
	}

	// init the decoder
	MFMADDecoder *pDecoder = (MFMADDecoder*)MFHeap_Alloc(sizeof(MFMADDecoder));
	pStream->pStreamData = pDecoder;
	pDecoder->pID3 = NULL;
	pDecoder->pFile = hFile;
	pDecoder->pGuardPtr = NULL;
	pDecoder->currentFrame = 0;
	pDecoder->frameCount = 0;
	pDecoder->overflowOffset = 0;
	pDecoder->overflowBytes = 0;
	pDecoder->pFrameOffsets = (uint32*)MFHeap_Alloc(sizeof(uint32)*2048);
	pDecoder->numFrameOffsetsAllocated = 2048;
	pDecoder->frameOffsetCount = 0;

	mad_stream_init(&pDecoder->stream);
	mad_frame_init(&pDecoder->frame);
	mad_synth_init(&pDecoder->synth);
	mad_header_init(&pDecoder->firstHeader);
	mad_timer_reset(&pDecoder->timer);

	// scan for frame offset(/s)
	bool bScanForHeaders = (pStream->createFlags & (MFASF_QueryLength | MFASF_AllowSeeking)) != 0;
	int sampleRate = ScanForFrames(pStream, pDecoder, !bScanForHeaders);
	pStream->trackLength = bScanForHeaders ? (float)(pDecoder->frameCount * SAMPLES_PER_MP3_FRAME) / (float)sampleRate : 0.0f;


	// prime the input buffer
	MFFile_Seek(hFile, pDecoder->pFrameOffsets[0], MFSeek_Begin);
	size_t read = MFFile_Read(hFile, pDecoder->inputBuffer, INPUT_BUFFER_SIZE, false);
	mad_stream_buffer(&pDecoder->stream, pDecoder->inputBuffer, (unsigned long)read);
	pDecoder->stream.error = MAD_ERROR_NONE;

	// decode the first frame so we can get the frame header
	GetMADSamples(pStream, NULL, 0);

	// fill out the stream info
	pStream->streamInfo.sampleRate = pDecoder->firstHeader.samplerate;
	pStream->streamInfo.channels = 2;
	pStream->streamInfo.bitsPerSample = 16;
	pStream->streamInfo.bufferLength = pDecoder->firstHeader.samplerate;
}

void SeekMADStream(MFAudioStream *pStream, float seconds)
{
	MFMADDecoder *pDecoder = (MFMADDecoder*)pStream->pStreamData;

	// seeking mp3's is a bitch!!!
	uint32 sample = (uint32)((float)pDecoder->firstHeader.samplerate * seconds);
	uint32 frame = sample / SAMPLES_PER_MP3_FRAME;
	uint32 sampleInFrame = sample % SAMPLES_PER_MP3_FRAME;

//	uint32 frameRecord = frame/10;
	uint32 frameRecord = frame;
	if(frameRecord >= pDecoder->frameOffsetCount)
		frameRecord = pDecoder->frameOffsetCount-1;
//	pDecoder->currentFrame = frameRecord*10;
	pDecoder->currentFrame = frameRecord;

	// seek to frame
	uint32 frameOffset = pDecoder->pFrameOffsets[frameRecord];
	MFFile_Seek(pDecoder->pFile, frameOffset, MFSeek_Begin);

	// prime input buffer
	size_t bytes = MFFile_Read(pDecoder->pFile, pDecoder->inputBuffer, INPUT_BUFFER_SIZE, false);
	mad_stream_buffer(&pDecoder->stream, pDecoder->inputBuffer, (unsigned long)bytes);
	pDecoder->stream.error = MAD_ERROR_NONE;
	pDecoder->overflowBytes = 0;

	// initialise timer to our song position
	pDecoder->timer.seconds = 0;
	pDecoder->timer.fraction = MAD_TIMER_RESOLUTION / pDecoder->firstHeader.samplerate;
	mad_timer_multiply(&pDecoder->timer, pDecoder->currentFrame*SAMPLES_PER_MP3_FRAME);

	// skip samples to where we want to be..
	int skipFrames = frame - pDecoder->currentFrame;
	int skipSamples = skipFrames*SAMPLES_PER_MP3_FRAME + sampleInFrame;
	int skipBytes = skipSamples * 4;
	GetMADSamples(pStream, NULL, skipBytes);
}

float GetMADTime(MFAudioStream *pStream)
{
	MFMADDecoder *pDecoder = (MFMADDecoder*)pStream->pStreamData;
	return (float)mad_timer_count(pDecoder->timer, MAD_UNITS_MILLISECONDS) * 0.001f;
}

void MFSound_RegisterMAD()
{
	MFStreamCallbacks callbacks;
	callbacks.pCreateStream = CreateMADStream;
	callbacks.pDestroyStream = DestroyMADStream;
	callbacks.pGetSamples = GetMADSamples;
	callbacks.pGetTime = GetMADTime;
	callbacks.pSeekStream = SeekMADStream;

	MFSound_RegisterStreamHandler("MP3 Audio (MAD)", ".mp3", &callbacks);
}
#endif
