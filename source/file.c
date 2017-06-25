#include "file.h"

#include "minizip/unzip.h"

#include "7z/7z.h"
#include "7z/7zAlloc.h"
#include "7z/7zCrc.h"
#include "7z/7zMemInStream.h"

Result openFile(const char * path, Handle * filehandle, bool write)
{
	FS_Archive archive = (FS_Archive){ARCHIVE_SDMC};
	FS_Path emptyPath = fsMakePath(PATH_ASCII, "");
	u32 flags = (write ? (FS_OPEN_CREATE | FS_OPEN_WRITE) : FS_OPEN_READ);
	int prefixlen = 0;
	
	if (!strncmp(path, "ctrnand:/", 9)) {
		archive = (FS_Archive){ARCHIVE_NAND_CTR_FS};
		prefixlen = 8;
	}
	else if (!strncmp(path, "twlp:/", 6)) {
		archive = (FS_Archive){ARCHIVE_TWL_PHOTO};
		prefixlen = 5;
	}
	else if (!strncmp(path, "twln:/", 6)) {
		archive = (FS_Archive){ARCHIVE_NAND_TWL_FS};
		prefixlen = 5;
	}
	else if (!strncmp(path, "sdmc:/", 6)) {
		prefixlen = 5;
	}
	
	FS_Path filePath = fsMakePath(PATH_ASCII, path+prefixlen);
	Result ret = FSUSER_OpenFileDirectly(filehandle, archive, emptyPath, filePath, flags, 0);
	if (ret != 0) printf("Error in:\nFSUSER_OpenFileDirectly\n");
	
	return ret;
}

Result copyFile(const char * srcpath, const char * destpath)
{
	if (srcpath == NULL || destpath == NULL) {
		printf("Can't copy, path is empty.\n");
		return -1;
	}
	
	Handle filehandle;
	Result ret = openFile(srcpath, &filehandle, false);
	if (ret != 0) {
		printf("Can't copy, couldn't open source file.\n");
		return -2;
	}
	
	FILE *destptr = fopen(destpath, "wb");
	if (destptr == NULL) {
		printf("Couldnt open destination file to write.\n");
		return -3;
	}
	
	printf("Copying:\n%s\nto:\n%s\n", srcpath, destpath);
	
	u8 * buf = malloc(0x1000);
	u32 bytesRead = 0;
	u64 offset = 0;
	
	do {
		readFile(filehandle, &bytesRead, offset, buf, 0x1000);
		fwrite(buf, 1, bytesRead, destptr);
		offset += bytesRead;
	} while(bytesRead);
	
	printf("Copied %llu bytes.\n", offset);
	
	closeFile(filehandle);
	fclose(destptr);
	free(buf);
	
	return 0;
}

Result extractFileFrom7z(const char * archive_file, const char * filename, const char * filepath)
{	
	if (filename == NULL) {
		printf("Cannot start, the inarchive in config.json is blank.\n");
		return EXTRACTION_ERROR_CONFIG;
	}
	
	if (filepath == NULL) {
		printf("Cannot start, the path in config.json is blank.\n");
		return EXTRACTION_ERROR_CONFIG;
	}
	
	Result ret = 0;
	
	FILE * archive = fopen(archive_file, "rb");
	if (archive == NULL) {
		printf("Error: couldn't open archive to read.\n");
		return EXTRACTION_ERROR_ARCHIVE_OPEN;
	}
	
	fseek(archive, 0, SEEK_END);
	u32 archiveSize = ftell(archive);
	fseek(archive, 0, SEEK_SET);
	
	u8 * archiveData = malloc(archiveSize);
	fread(archiveData, archiveSize, 1, archive);
	
	CMemInStream memStream;
	CSzArEx db;
	ISzAlloc allocImp;
	ISzAlloc allocTempImp;
	
	MemInStream_Init(&memStream, archiveData, archiveSize);
	
	allocImp.Alloc = SzAlloc;
	allocImp.Free = SzFree;
	allocTempImp.Alloc = SzAllocTemp;
	allocTempImp.Free = SzFreeTemp;
	
	CrcGenerateTable();
	SzArEx_Init(&db);
	
	SRes res = SzArEx_Open(&db, &memStream.s, &allocImp, &allocTempImp);
	if (res != SZ_OK) {
		ret = EXTRACTION_ERROR_ARCHIVE_OPEN;
		goto finish;
	}
	
	for (u32 i = 0; i < db.NumFiles; ++i) {
		// Skip directories
		unsigned isDir = SzArEx_IsDir(&db, i);
		if (isDir) continue;
		
		// Get name
		size_t len;
		len = SzArEx_GetFileNameUtf16(&db, i, NULL);
		// Super long filename? Just skip it..
		if (len >= 256) continue;
		
		u16 name[256] = {0};
		SzArEx_GetFileNameUtf16(&db, i, name);
		
		// Convert name to ASCII (just cut the other bytes)
		char name8[256] = {0};
		for (size_t j = 0; j < len; ++j) {
			name8[j] = name[j] % 0xff;
		}
		
		u8 * buf = NULL;
		UInt32 blockIndex = UINT32_MAX;
		size_t fileBufSize = 0;
		size_t offset = 0;
		size_t fileSize = 0;

		if (!strcmp(name8, filename)) {
			res = SzArEx_Extract(
						&db,
						&memStream.s,
						i,
						&blockIndex,
						&buf,
						&fileBufSize,
						&offset,
						&fileSize,
						&allocImp,
						&allocTempImp
			);
			if (res != SZ_OK) {
				printf("Error: Couldn't extract file data from archive.\n");
				ret = EXTRACTION_ERROR_READ_IN_ARCHIVE;
				goto finish;
			}
			
			Handle filehandle;
			u32 bytesWritten = 0;
			u64 offset = 0;
			
			ret = openFile(filepath, &filehandle, true);
			if (ret != 0) {
				printf("Error: couldn't open file to write.\n");
				return EXTRACTION_ERROR_WRITEFILE;
			}
			
			ret = writeFile(filehandle, &bytesWritten, offset, buf+offset, (u32)fileSize);
			closeFile(filehandle);
			
			free(buf);
			
			goto finish;
		}
	}
	
	ret = EXTRACTION_ERROR_FIND;
	
	finish:
	free(archiveData);
	fclose(archive);
	
	return ret;
}

Result extractFileFromZip(const char * archive_file, const char * filename, const char * filepath)
{	
	if (filename == NULL) {
		printf("Cannot start, the inarchive in config.json is blank.\n");
		return EXTRACTION_ERROR_CONFIG;
	}
	
	if (filepath == NULL) {
		printf("Cannot start, the path in config.json is blank.\n");
		return EXTRACTION_ERROR_CONFIG;
	}
	
	Result ret = 0;
	u8 * buf = NULL;
	Handle filehandle;
	u32 bytesWritten = 0;
	u64 offset = 0;
	
	ret = openFile(filepath, &filehandle, true);
	if (ret != 0) {
		printf("Error: couldn't open file to write.\n");
		return EXTRACTION_ERROR_WRITEFILE;
	}
	
	unzFile uf = unzOpen64(archive_file);
	if (uf == NULL) {
		printf("Couldn't open zip file.\n");
		closeFile(filehandle);
		return EXTRACTION_ERROR_ARCHIVE_OPEN;
	}
	
	unz_file_info payloadInfo = {};
	
	ret = unzLocateFile(uf, filename, NULL);

	if (ret == UNZ_END_OF_LIST_OF_FILE) {
		printf("Couldn't find the wanted file in the zip.\n");
		ret = EXTRACTION_ERROR_FIND;
	}
	else if (ret == UNZ_OK) {
		ret = unzGetCurrentFileInfo(uf, &payloadInfo, NULL, 0, NULL, 0, NULL, 0);
		if (ret != UNZ_OK) {
			printf("Couldn't get the file information.\n");
			ret = EXTRACTION_ERROR_INFO;
			goto finish;
		}
		
		u32 toRead = 0x1000;
		buf = malloc(toRead);
		if (buf == NULL) {
			ret = EXTRACTION_ERROR_ALLOC;
			goto finish;
		}
		
		ret = unzOpenCurrentFile(uf);
		if (ret != UNZ_OK) {
			ret = EXTRACTION_ERROR_OPEN_IN_ARCHIVE;
			goto finish;
		}
		
		u32 size = payloadInfo.uncompressed_size;
		
		do {
			if (size < toRead) toRead = size;
			ret = unzReadCurrentFile(uf, buf, toRead);
			if (ret > 0) {
				ret = writeFile(filehandle, &bytesWritten, offset, buf, toRead);
				offset += toRead;
			}
			else {
				ret = EXTRACTION_ERROR_READ_IN_ARCHIVE;
				goto finish;
			}
			size -= toRead;
		} while(size);
	}
	
	finish:
	free(buf);
	unzClose(uf);
	
	closeFile(filehandle);
	
	return ret;
}

Result extractFileFromArchive(const char * archive_path, const char * filename, const char * filepath)
{
	Result ret = EXTRACTION_ERROR_ARCHIVE_OPEN;
	
	unzFile uf = unzOpen64(archive_path);
	if (uf == NULL) {
		printf("Opening as a 7z file...\n");
		//failure to open as a zip -> must be 7z
		ret = extractFileFrom7z(archive_path, filename, filepath);
	}
	else {
		printf("Opening as a zip file...\n");
		unzClose(uf);
		ret = extractFileFromZip(archive_path, filename, filepath);
	}
	
	remove(archive_path);
	return ret;
}
