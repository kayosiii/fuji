#include "Fuji_Internal.h"

#if MF_FILESYSTEM == MF_DRIVER_WIN32

#if defined(MF_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#elif defined(MF_XBOX)
	#include <xtl.h>

	char *FixXBoxFilename(const char *pFilename);
#endif

#include "MFHeap.h"
#include "MFFileSystem_Internal.h"
#include "FileSystem/MFFileSystemNative_Internal.h"

MFInitStatus MFFileSystemNative_InitModulePlatformSpecific()
{
	return MFIS_Succeeded;
}

void MFFileSystemNative_Register()
{

}

void MFFileSystemNative_Unregister()
{

}

static void CreateDir(const char *pPath)
{
	if(!pPath || *pPath == 0)
		return;

	// strip trailing '/'
	((char*)pPath)[MFString_Length(pPath)-1] = 0;

	// the path is empty
	if(*pPath == 0)
		return;

	CreateDir(MFStr_GetFilePath(pPath));

	CreateDirectory(pPath, NULL);
}

int MFFileNative_Open(MFFile *pFile, MFOpenData *pOpenData)
{
	MFCALLSTACK;

	MFDebug_Assert(pOpenData->cbSize == sizeof(MFOpenDataNative), "Incorrect size for MFOpenDataNative structure. Invalid pOpenData.");
	MFOpenDataNative *pNative = (MFOpenDataNative*)pOpenData;

	MFDebug_Assert(pOpenData->openFlags & (MFOF_Read|MFOF_Write), "Neither MFOF_Read nor MFOF_Write specified.");
	MFDebug_Assert((pNative->openFlags & (MFOF_Text|MFOF_Binary)) != (MFOF_Text|MFOF_Binary), "MFOF_Text and MFOF_Binary are mutually exclusive.");

	const char *pFilename = pNative->pFilename;
#if defined(MF_XBOX) || defined(MF_X360)
	pFilename = FixXBoxFilename(pFilename);
#endif

	if(pOpenData->openFlags & MFOF_CreateDirectory)
		CreateDir(MFStr_GetFilePath(pFilename));

	DWORD access = 0, create = 0;

	// set the access flags and create mode
	if(pOpenData->openFlags & MFOF_Read)
		access |= GENERIC_READ;
	if(pOpenData->openFlags & (MFOF_Write|MFOF_Append))
	{
		if(pNative->openFlags & MFOF_Append)
			access |= FILE_APPEND_DATA;
		else
			access |= GENERIC_WRITE;

		if(pNative->openFlags & MFOF_Truncate)
			create = CREATE_ALWAYS;
		else
			create = OPEN_ALWAYS;
	}
	else
		create = OPEN_EXISTING;

	pFile->pFilesysData = CreateFile(pFilename, access, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, create, FILE_ATTRIBUTE_NORMAL, NULL);

	if(pFile->pFilesysData == INVALID_HANDLE_VALUE)
	{
		pFile->pFilesysData = 0;
		return -1;
	}

	LARGE_INTEGER size;
	if(!GetFileSizeEx(pFile->pFilesysData, &size))
	{
		MFDebug_Warn(2, MFStr("Call: MFFileNative_Open() - Error: Couldn't get size for file '%s'.", pNative->pFilename));
		CloseHandle(pFile->pFilesysData);
		pFile->pFilesysData = 0;
		return -1;
	}

	pFile->createFlags = pOpenData->openFlags;
	pFile->offset = 0;
	pFile->length = size.QuadPart;

	MFString_Copy(pFile->fileIdentifier, pNative->pFilename);

	return 0;
}

int MFFileNative_Close(MFFile* fileHandle)
{
	MFCALLSTACK;

	CloseHandle(fileHandle->pFilesysData);

	return 0;
}

size_t MFFileNative_Read(MFFile* fileHandle, void *pBuffer, size_t bytes)
{
	MFCALLSTACK;

#if defined(MF_64BIT)
	// loop since read only supports 32 bits
	size_t totalRead = 0;
	while(bytes)
	{
		DWORD bytesRead, toRead = (DWORD)bytes;
		ReadFile(fileHandle->pFilesysData, (char*)pBuffer + totalRead, toRead, &bytesRead, NULL);
		fileHandle->offset += bytesRead;
		totalRead += bytesRead;

		// if we can't read any more data, bail out of loop
		if(bytesRead < toRead)
			break;

		bytes -= bytesRead;
	}

	return totalRead;
#else
	DWORD bytesRead;
	ReadFile(fileHandle->pFilesysData, pBuffer, (DWORD)bytes, &bytesRead, NULL);
	fileHandle->offset += bytesRead;
	return (size_t)bytesRead;
#endif
}

size_t MFFileNative_Write(MFFile* fileHandle, const void *pBuffer, size_t bytes)
{
	MFCALLSTACK;

	// TODO: support 64bit properly!
	// if size_t is 64bit, and a large value is passed to 'bytes', this will fail...

	DWORD bytesWritten;

	if(fileHandle->createFlags & MFOF_Append)
	{
		// seek to end
		LARGE_INTEGER rel;
		rel.QuadPart = 0;
		SetFilePointerEx(fileHandle->pFilesysData, rel, NULL, FILE_END);

		WriteFile(fileHandle->pFilesysData, pBuffer, (DWORD)bytes, (LPDWORD)&bytesWritten, NULL);
		fileHandle->length += bytesWritten;

		// return to current position
		rel.QuadPart = fileHandle->offset;
		SetFilePointerEx(fileHandle->pFilesysData, rel, NULL, FILE_BEGIN);
	}
	else
	{
		WriteFile(fileHandle->pFilesysData, pBuffer, (DWORD)bytes, (LPDWORD)&bytesWritten, NULL);
		fileHandle->offset += bytesWritten;
		fileHandle->length = MFMax(fileHandle->offset, fileHandle->length);
	}

	return (size_t)bytesWritten;
}

uint64 MFFileNative_Seek(MFFile* fileHandle, int64 bytes, MFFileSeek relativity)
{
	MFCALLSTACK;

	MFDebug_Assert(fileHandle->pFilesysData, "MFFileNative_Seek() - Invalid file!");

	DWORD method = 0;

	switch(relativity)
	{
		case MFSeek_Begin:
			method = FILE_BEGIN;
			break;
		case MFSeek_End:
			method = FILE_END;
			break;
		case MFSeek_Current:
			method = FILE_CURRENT;
			break;
		default:
			MFDebug_Assert(false, "MFFileNative_Seek() - Invalid 'relativity' for file seeking.");
	}

	LARGE_INTEGER rel, pos;
	rel.QuadPart = bytes;
	if(!SetFilePointerEx(fileHandle->pFilesysData, rel, &pos, method))
	{
		MFDebug_Warn(1, "MFFileNative_Seek() - SetFilePointerEx() failed!");

		rel.QuadPart = 0;
		if(!SetFilePointerEx(fileHandle->pFilesysData, rel, &pos, FILE_CURRENT))
		{
			MFDebug_Assert(false, "Failed to set file position!");
			return fileHandle->offset;
		}
	}

	fileHandle->offset = pos.QuadPart;
	return (uint64)pos.QuadPart;
}

uint64 MFFileNative_GetSize(const char* pFilename)
{
	MFCALLSTACK;

	uint64 fileSize = 0;

#if defined(MF_XBOX) || defined(MF_X360)
	pFilename = FixXBoxFilename(pFilename);
#endif

	HANDLE hFile = CreateFile(pFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		LARGE_INTEGER size;
		GetFileSizeEx(hFile, &size);
		CloseHandle(hFile);

		fileSize = size.QuadPart;
	}

	return fileSize;
}

bool MFFileNative_Exists(const char* pFilename)
{
	MFCALLSTACK;

	bool exists = false;

#if defined(MF_XBOX) || defined(MF_X360)
	pFilename = FixXBoxFilename(pFilename);
#endif

	HANDLE hFile = CreateFile(pFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL);

	if(hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
		exists = true;
	}

	return exists;
}

const char* MFFileNative_MakeAbsolute(const char* pFilename)
{
	char path[256];
	GetFullPathName(pFilename, sizeof(path), path, NULL);
	return MFStr(path);
}

bool MFFileNative_FindFirst(MFFind *pFind, const char *pSearchPattern, MFFindData *pFindData)
{
	WIN32_FIND_DATA fd;

	HANDLE hFind = FindFirstFile(MFStr("%s%s", (char*)pFind->pMount->pFilesysData, pSearchPattern), &fd);

	if(hFind == INVALID_HANDLE_VALUE)
		return false;

	BOOL more = TRUE;
	while(!MFString_Compare(fd.cFileName, ".") || !MFString_Compare(fd.cFileName, "..") && more)
		more = FindNextFile(hFind, &fd);
	if(!more)
	{
		FindClose(hFind);
		return false;
	}

	pFindData->attributes = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? MFFA_Directory : 0;
	pFindData->fileSize = (uint64)fd.nFileSizeLow | (((uint64)fd.nFileSizeHigh) << 32);
	MFString_Copy((char*)pFindData->pFilename, fd.cFileName);

	MFString_CopyCat(pFindData->pSystemPath, (char*)pFind->pMount->pFilesysData, pSearchPattern);
	char *pLast = MFString_RChr(pFindData->pSystemPath, '/');
	if(pLast)
		pLast[1] = 0;
	else
		pFindData->pSystemPath[0] = NULL;

	pFind->pFilesystemData = (void*)hFind;

	return true;
}

bool MFFileNative_FindNext(MFFind *pFind, MFFindData *pFindData)
{
	WIN32_FIND_DATA fd;

	BOOL more = FindNextFile((HANDLE)pFind->pFilesystemData, &fd);

	if(!more)
		return false;

	pFindData->attributes = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? MFFA_Directory : 0;
	pFindData->fileSize = (uint64)fd.nFileSizeLow | (((uint64)fd.nFileSizeHigh) << 32);
	MFString_Copy((char*)pFindData->pFilename, fd.cFileName);

	return true;
}

void MFFileNative_FindClose(MFFind *pFind)
{
	FindClose((HANDLE)pFind->pFilesystemData);
}

#endif // MF_FILESYSTEM
