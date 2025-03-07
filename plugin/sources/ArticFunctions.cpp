#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ArticFunctions.hpp"
#include "Main.hpp"
#include "CTRPluginFramework/CTRPluginFramework.hpp"
#include "CTRPluginFramework/Clock.hpp"
#include "nim_extheader.h"

extern "C" {
#include "csvc.h"
}

extern bool isControllerMode;
constexpr u32 INITIAL_SETUP_APP_VERSION = 0;

enum class HandleType {
    FILE,
    DIR,
    ARCHIVE
};

namespace ArticFunctions {

    ExHeader_Info lastAppExheader;
    std::map<u64, HandleType> openHandles;
    CTRPluginFramework::Mutex amMutex;
    CTRPluginFramework::Mutex cfgMutex;

    void Process_GetTitleID(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* tid_buffer = mi.ReserveResultBuffer(0, sizeof(u64));
        if (!tid_buffer) {
            return;
        }
        s64 out;
        svcGetProcessInfo(&out, CUR_PROCESS_HANDLE, 0x10001);

        memcpy(tid_buffer->data, &out, sizeof(s64));

        mi.FinishGood(0);
    }

    void Process_GetProductInfo(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* prod_code_buffer = mi.ReserveResultBuffer(0, sizeof(FS_ProductInfo));
        if (!prod_code_buffer) {
            return;
        }
        
        u32 pid;
        Result res = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
        if (R_SUCCEEDED(res)) res = FSUSER_GetProductInfo((FS_ProductInfo*)prod_code_buffer->data, pid);
        if (R_FAILED(res)) {
            logger.Error("Process_GetProductInfo: 0x%08X", res);
            mi.FinishInternalError();
            return;
        }

        mi.FinishGood(0);
    }

    void Process_GetExheader(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* exheader_buf = mi.ReserveResultBuffer(0, sizeof(lastAppExheader));
        if (!exheader_buf) {
            return;
        }
        memcpy(exheader_buf->data, &lastAppExheader, exheader_buf->bufferSize);

        mi.FinishGood(0);
    }

    void Process_ReadCode(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 offset, size;

        if (good) good = mi.GetParameterS32(offset);
        if (good) good = mi.GetParameterS32(size);
        if (good) good = mi.FinishInputParameters();
        if (!good) return;

        s64 out;
        if (R_FAILED(svcGetProcessInfo(&out, CUR_PROCESS_HANDLE, 0x10005))) {
            mi.FinishInternalError();
            return;
        }
        u8* start_addr = reinterpret_cast<u8*>(out);

        ArticProtocolCommon::Buffer* code_buf = mi.ReserveResultBuffer(0, size);
        if (!code_buf) {
            return;
        }
        memcpy(code_buf->data, start_addr + offset, size);

        mi.FinishGood(0);
    }

    static void _Process_ReadExefs(ArticProtocolServer::MethodInterface& mi, const char* section) {
        bool good = true;

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        // Set up FS_Path structures
        u8 path[0xC] = {0};
        u32* type = (u32*)path;
        char* name = (char*)(path + sizeof(u32));
        
        *type = 0x2; // ExeFS
        strcpy(name, section); // Icon

        FS_Path archPath = { PATH_EMPTY, 1, "" };
        FS_Path filePath = { PATH_BINARY, sizeof(path), path };

        // Open the RomFS file and mount it
        Handle fd = 0;
        Result rc = FSUSER_OpenFileDirectly(&fd, ARCHIVE_ROMFS, archPath, filePath, FS_OPEN_READ, 0);
        if (R_FAILED(rc)) {
            mi.FinishGood(rc);
            return;
        }

        u64 file_size;
        rc = FSFILE_GetSize(fd, &file_size);
        if (R_FAILED(rc)) {
            FSFILE_Close(fd);
            mi.FinishGood(rc);
            return;
        }

        ArticProtocolCommon::Buffer* icon_buf = mi.ReserveResultBuffer(0, static_cast<size_t>(file_size));
        if (!icon_buf) {
            FSFILE_Close(fd);
            return;
        }

        u32 bytes_read;
        rc = FSFILE_Read(fd, &bytes_read, 0, icon_buf->data, icon_buf->bufferSize);
        if (R_FAILED(rc)) {
            FSFILE_Close(fd);
            mi.ResizeLastResultBuffer(icon_buf, 0);
            mi.FinishGood(rc);
            return;
        }

        mi.ResizeLastResultBuffer(icon_buf, bytes_read);
        FSFILE_Close(fd);

        mi.FinishGood(0);
    }

    void Process_ReadIcon(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "icon");
    }

    void Process_ReadBanner(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "banner");
    }

    void Process_ReadLogo(ArticProtocolServer::MethodInterface& mi) {
        _Process_ReadExefs(mi, "logo");
    }

    bool GetFSPath(ArticProtocolServer::MethodInterface& mi, FS_Path& path) {
        void* pathPtr; size_t pathSize;
        
        if (!mi.GetParameterBuffer(pathPtr, pathSize))
            return false;

        path.type = reinterpret_cast<FS_Path*>(pathPtr)->type;
        path.size = reinterpret_cast<FS_Path*>(pathPtr)->size;
        if (pathSize < 8 || path.size != pathSize - 0x8) {
            mi.FinishInternalError();
            return false;
        }

        path.data = (u8*)pathPtr + 0x8;
        return true;
    }

    void FSUSER_OpenFileDirectly_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path archPath;
        FS_Path filePath;
        s32 openFlags;
        s32 attributes;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, archPath);
        if (good) good = GetFSPath(mi, filePath);
        if (good) good = mi.GetParameterS32(openFlags);
        if (good) good = mi.GetParameterS32(attributes);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenFileDirectly(&out, (FS_ArchiveID)archiveID, archPath, filePath, openFlags, attributes);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSFILE_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::FILE;

        mi.FinishGood(res);
    }

    void FSUSER_OpenArchive_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        s32 archiveID;
        FS_Path archPath;

        if (good) good = mi.GetParameterS32(archiveID);
        if (good) good = GetFSPath(mi, archPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        FS_Archive out;
        Result res = FSUSER_OpenArchive(&out, (FS_ArchiveID)archiveID, archPath);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(FS_Archive));
        if (!handle_buf) {
            FSUSER_CloseArchive(out);
            return;
        }

        *reinterpret_cast<FS_Archive*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::ARCHIVE;

        mi.FinishGood(res);
    }

    void FSUSER_CloseArchive_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSUSER_CloseArchive(archive);
        openHandles.erase((u64)archive);

        mi.FinishGood(res);
    }

    void FSUSER_OpenFile_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path filePath;
        s32 openFlags;
        s32 attributes;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, filePath);
        if (good) good = mi.GetParameterS32(openFlags);
        if (good) good = mi.GetParameterS32(attributes);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenFile(&out, archive, filePath, openFlags, attributes);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSFILE_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;

        // Citra always asks for the size after opening a file, provided it here.
        u64 fileSize;
        Result res2 = FSFILE_GetSize(out, &fileSize);
        if (R_SUCCEEDED(res2)) {
            ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(1, sizeof(u64));
            if (!size_buf) {
                FSFILE_Close(out);
                return;
            }

            *reinterpret_cast<u64*>(size_buf->data) = fileSize;
        }
        openHandles[(u64)out] = HandleType::FILE;

        mi.FinishGood(res);
    }

    void FSUSER_OpenDirectory_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        FS_Archive archive;
        FS_Path dirPath;

        if (good) good = mi.GetParameterS64(*reinterpret_cast<s64*>(&archive));
        if (good) good = GetFSPath(mi, dirPath);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle out;
        Result res = FSUSER_OpenDirectory(&out, archive, dirPath);

        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* handle_buf = mi.ReserveResultBuffer(0, sizeof(Handle));
        if (!handle_buf) {
            FSDIR_Close(out);
            return;
        }

        *reinterpret_cast<Handle*>(handle_buf->data) = out;
        openHandles[(u64)out] = HandleType::DIR;

        mi.FinishGood(res);
    }

    void FSFILE_Close_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSFILE_Close(handle);
        openHandles.erase((u64)handle);

        mi.FinishGood(res);
    }

    void FSFILE_GetSize_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u64 fileSize;
        Result res = FSFILE_GetSize(handle, &fileSize);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(0, sizeof(u64));
        if (!size_buf) {
            return;
        }

        *reinterpret_cast<u64*>(size_buf->data) = fileSize;
        mi.FinishGood(res);
    }

    void FSFILE_GetAttributes_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        u32 attributes;
        Result res = FSFILE_GetAttributes(handle, &attributes);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* size_buf = mi.ReserveResultBuffer(0, sizeof(u32));
        if (!size_buf) {
            return;
        }

        *reinterpret_cast<u32*>(size_buf->data) = attributes;
        mi.FinishGood(res);
    }

    void FSFILE_Read_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle, size;
        s64 offset;
        u32 bytes_read;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS64(offset);
        if (good) good = mi.GetParameterS32(size);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        logger.Debug("Read o=0x%08X, l=0x%08X", (u32)offset, (u32)size);

        ArticProtocolCommon::Buffer* read_buf = mi.ReserveResultBuffer(0, size);
        if (!read_buf) {
            return;
        }

        Result res = FSFILE_Read(handle, &bytes_read, offset, read_buf->data, read_buf->bufferSize);
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(read_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(read_buf, bytes_read);
        mi.FinishGood(res);
    }

    void FSDIR_Read_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;
        s32 entryCount;

        if (good) good = mi.GetParameterS32(handle);
        if (good) good = mi.GetParameterS32(entryCount);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        ArticProtocolCommon::Buffer* read_dir_buf = mi.ReserveResultBuffer(0, entryCount * sizeof(FS_DirectoryEntry));
        if (!read_dir_buf) {
            return;
        }

        u32 entries_read;
        Result res = FSDIR_Read(handle, &entries_read, entryCount, reinterpret_cast<FS_DirectoryEntry*>(read_dir_buf->data));
        if (R_FAILED(res)) {
            mi.ResizeLastResultBuffer(read_dir_buf, 0);
            mi.FinishGood(res);
            return;
        }

        mi.ResizeLastResultBuffer(read_dir_buf, entries_read * sizeof(FS_DirectoryEntry));
        mi.FinishGood(res);
    }

    void FSDIR_Close_(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s32 handle;

        if (good) good = mi.GetParameterS32(handle);

        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Result res = FSDIR_Close(handle);
        openHandles.erase((u64)handle);

        mi.FinishGood(res);
    }

    void System_IsAzaharInitialSetup(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;

        if (good) mi.FinishInputParameters();

        ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, 4);
        if (!ret_buf) {
            return;
        }
        reinterpret_cast<u32*>(ret_buf->data)[0] = INITIAL_SETUP_APP_VERSION;

        mi.FinishGood(0);
    }

    void System_GetSystemFile(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        s8 type;

        if (good) good = mi.GetParameterS8(type);
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        if (type < 0 || type > 5) {
            mi.FinishGood(-1);
            return;
        }

        // SecureInfo_A
        if (type >= 0 && type <= 2) {
            Handle fspxiHandle;
            Result res = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, &fspxiHandle, "PxiFS0");
            if (R_FAILED(res)) {
                mi.FinishGood(res);
                return;
            }
            FSPXI_Archive archive;
            res = FSPXI_OpenArchive(fspxiHandle, &archive, ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, ""));
            if (R_FAILED(res)) {
                svcCloseHandle(fspxiHandle);
                mi.FinishGood(res);
                return;
            }

            const char* files[] = {"/rw/sys/SecureInfo_", "/rw/sys/LocalFriendCodeSeed_", "/private/movable.sed"};
            char file_name[0x20] = {0};
            strcpy(file_name, files[type]);
            char* end = file_name + strlen(files[type]);

            if (type == 0 || type == 1) {
                *end = 'A';
            }

            FSPXI_File file;
            res = FSPXI_OpenFile(fspxiHandle, &file, archive, fsMakePath(PATH_ASCII, file_name), FS_OPEN_READ, 0);
            if (R_FAILED(res)) {
                *end = 'B';
                res = FSPXI_OpenFile(fspxiHandle, &file, archive, fsMakePath(PATH_ASCII, file_name), FS_OPEN_READ, 0);
                if (R_FAILED(res)) {
                    FSPXI_CloseArchive(fspxiHandle, archive);
                    svcCloseHandle(fspxiHandle);
                    mi.FinishGood(res);
                    return;
                }
            }

            u64 size = 0;
            res = FSPXI_GetFileSize(fspxiHandle, file, &size);
            if (R_FAILED(res)) {
                FSPXI_CloseFile(fspxiHandle, file);
                FSPXI_CloseArchive(fspxiHandle, archive);
                svcCloseHandle(fspxiHandle);
                mi.FinishGood(res);
                return;
            }

            ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, (int)size);
            if (!ret_buf) {
                FSPXI_CloseFile(fspxiHandle, file);
                FSPXI_CloseArchive(fspxiHandle, archive);
                svcCloseHandle(fspxiHandle);
                return;
            }

            u32 bytes_read = 0;
            res = FSPXI_ReadFile(fspxiHandle, file, &bytes_read, 0, ret_buf->data, (u32)size);
            FSPXI_CloseFile(fspxiHandle, file);
            FSPXI_CloseArchive(fspxiHandle, archive);
            svcCloseHandle(fspxiHandle);
            mi.FinishGood(bytes_read != size ? -2 : res);
        } else if (type == 3) {
            Result res = amInit();
            if (R_FAILED(res)) {
                mi.FinishGood(res);
                return;
            }

            u32 deviceID;
            res = AM_GetDeviceId(&deviceID);
            amExit();
            if (R_FAILED(res)) {
                mi.FinishGood(res);
                return;
            }
            char filePath[0x50];
            sprintf(filePath, "/luma/backups/%08lX/otp.bin", deviceID);
            
            Handle file;
            
            res = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, filePath), FS_OPEN_READ, 0);
            if (R_FAILED(res)) {
                logger.Error("Missing OTP backup on SD card, please update your luma version and/or remove the console battery.");
                mi.FinishGood(res);
                return;
            }

            u64 size = 0;
            res = FSFILE_GetSize(file, &size);
            if (R_FAILED(res)) {
                FSFILE_Close(file);
                mi.FinishGood(res);
                return;
            }

            ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, (int)size);
            if (!ret_buf) {
                FSFILE_Close(file);
                return;
            }

            u32 bytes_read = 0;
            res = FSFILE_Read(file, &bytes_read, 0, ret_buf->data, (u32)size);
            FSFILE_Close(file);
            mi.FinishGood(bytes_read != size ? -2 : res);
        } else if (type == 4) {
            Result res = cfguInit();
            if (R_FAILED(res)) {
                mi.FinishGood(res);
            }

            u64 consoleID = 0;
            u32 random = 0;
            res = CFGU_GetConfigInfoBlk2(0x8, 0x00090001, &consoleID);
            if (R_SUCCEEDED(res)) res = CFGU_GetConfigInfoBlk2(0x4, 0x00090002, &random);
            if (R_FAILED(res)) {
                cfguExit();
                mi.FinishGood(res);
            }

            ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, 0xC);
            if (!ret_buf) {
                return;
            }

            *reinterpret_cast<u64*>(ret_buf->data) = consoleID;
            *reinterpret_cast<u32*>(ret_buf->data + 8) = random;

            mi.FinishGood(res);
        } else if (type == 5) {
            ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, 6);
            if (!ret_buf) {
                return;
            }

            memcpy(ret_buf->data, OS_SharedConfig->wifi_macaddr, 6);

            mi.FinishGood(0);
        }
    }

    static u32 getle32(const u8* p)
    {
        return (p[0]<<0) | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
    }

    static u32 lzss_get_decompressed_size(u8* compressed, u32 compressedsize)
    {
        u8* footer = compressed + compressedsize - 8;

        u32 originalbottom = getle32(footer+4);

        return originalbottom + compressedsize;
    }

    static int lzss_decompress(u8* compressed, u32 compressedsize, u8* decompressed, u32 decompressedsize)
    {
        u8* footer = compressed + compressedsize - 8;
        u32 buffertopandbottom = getle32(footer+0);
        //u32 originalbottom = getle32(footer+4);
        u32 i, j;
        u32 out = decompressedsize;
        u32 index = compressedsize - ((buffertopandbottom>>24)&0xFF);
        u32 segmentoffset;
        u32 segmentsize;
        u8 control;
        u32 stopindex = compressedsize - (buffertopandbottom&0xFFFFFF);

        memset(decompressed, 0, decompressedsize);
        memcpy(decompressed, compressed, compressedsize);

        
        while(index > stopindex)
        {
            control = compressed[--index];
            

            for(i=0; i<8; i++)
            {
                if (index <= stopindex)
                    break;

                if (index <= 0)
                    break;

                if (out <= 0)
                    break;

                if (control & 0x80)
                {
                    if (index < 2)
                    {
                        // fprintf(stderr, "Error, compression out of bounds\n");
                        goto clean;
                    }

                    index -= 2;

                    segmentoffset = compressed[index] | (compressed[index+1]<<8);
                    segmentsize = ((segmentoffset >> 12)&15)+3;
                    segmentoffset &= 0x0FFF;
                    segmentoffset += 2;

                    
                    if (out < segmentsize)
                    {
                        // fprintf(stderr, "Error, compression out of bounds\n");
                        goto clean;
                    }

                    for(j=0; j<segmentsize; j++)
                    {
                        u8 data;
                        
                        if (out+segmentoffset >= decompressedsize)
                        {
                            // fprintf(stderr, "Error, compression out of bounds\n");
                            goto clean;
                        }

                        data  = decompressed[out+segmentoffset];
                        decompressed[--out] = data;
                    }
                }
                else
                {
                    if (out < 1)
                    {
                        // fprintf(stderr, "Error, compression out of bounds\n");
                        goto clean;
                    }
                    decompressed[--out] = compressed[--index];
                }

                control <<= 1;
            }
        }

        return 0;
        
        clean:
        return -1;
    }

    void System_GetNIM(ArticProtocolServer::MethodInterface& mi) {
        bool good = true;
        
        if (good) good = mi.FinishInputParameters();

        if (!good) return;

        Handle file;
        u32 archive_path[4] = {0x00002C02, 0x00040130, MEDIATYPE_NAND, 0x0};
        u32 file_path[5] = {0x0, 0x0, 0x2, 0x646F632E, 0x00000065};
        FS_Path archive_path_bin = {PATH_BINARY, 0x10, archive_path};
        FS_Path file_path_bin = {PATH_BINARY, 0x14, file_path};
        Result res = FSUSER_OpenFileDirectly(&file, ARCHIVE_SAVEDATA_AND_CONTENT, archive_path_bin, file_path_bin, FS_OPEN_READ, 0);
        if (R_FAILED(res)) {
            mi.FinishGood(res);
            return;
        }

        u64 size = 0;
        res = FSFILE_GetSize(file, &size);
        if (R_FAILED(res)) {
            FSFILE_Close(file);
            mi.FinishGood(res);
            return;
        }

        u8* buffer = (u8*)malloc((size_t)size);
        u32 bytes_read = 0;
        res = FSFILE_Read(file, &bytes_read, 0, buffer, (u32)size);
        FSFILE_Close(file);
        if (R_FAILED(res) || bytes_read != size) {
            if (bytes_read != size) res = -2;
            free(buffer);
            mi.FinishGood(res);
            return;
        }

        ArticProtocolCommon::Buffer* ret_buf = mi.ReserveResultBuffer(0, nim_extheader_bin_size);
        if (!ret_buf) {
            free(buffer);
            return;
        }
        memcpy(ret_buf->data, nim_extheader_bin, nim_extheader_bin_size);
        ret_buf = mi.ReserveResultBuffer(1, lzss_get_decompressed_size(buffer, (u32)size));
        if (!ret_buf) {
            free(buffer);
            return;
        }
        lzss_decompress(buffer, (u32)size, (u8*)ret_buf->data, ret_buf->bufferSize);
        free(buffer);

        u64 checksum = 0;
        u64* start = (u64*)(ret_buf->data), *end = (u64*)((uintptr_t)(ret_buf->data + ret_buf->bufferSize) & ~7);
        while (start != end) checksum = (checksum + *start++) * 0x6500000065ULL;
        if (checksum != 0x50F9D326AB2239E9ULL) {
            logger.Error("Invalid NIM checksum. Please ensure your console is on the latest version");
            mi.ResizeLastResultBuffer(ret_buf, 0);
            mi.FinishGood(-3);
            return;
        }

        mi.FinishGood(0);
    }

    template<std::size_t N>
    constexpr auto& METHOD_NAME(char const (&s)[N]) {
        static_assert(N < sizeof(ArticProtocolCommon::RequestPacket::method), "String exceeds 32 bytes!");
        return s;
    }

    std::map<std::string, void(*)(ArticProtocolServer::MethodInterface& mi)> functionHandlers = {
        {METHOD_NAME("Process_GetTitleID"), Process_GetTitleID},
        {METHOD_NAME("Process_GetProductInfo"), Process_GetProductInfo},
        {METHOD_NAME("Process_GetExheader"), Process_GetExheader},
        {METHOD_NAME("Process_ReadCode"), Process_ReadCode},
        {METHOD_NAME("Process_ReadIcon"), Process_ReadIcon},
        {METHOD_NAME("Process_ReadBanner"), Process_ReadBanner},
        {METHOD_NAME("Process_ReadLogo"), Process_ReadLogo},
        {METHOD_NAME("FSUSER_OpenFileDirectly"), FSUSER_OpenFileDirectly_},
        {METHOD_NAME("FSUSER_OpenArchive"), FSUSER_OpenArchive_},
        {METHOD_NAME("FSUSER_CloseArchive"), FSUSER_CloseArchive_},
        {METHOD_NAME("FSUSER_OpenFile"), FSUSER_OpenFile_},
        {METHOD_NAME("FSUSER_OpenDirectory"), FSUSER_OpenDirectory_},
        {METHOD_NAME("FSFILE_Close"), FSFILE_Close_},
        {METHOD_NAME("FSFILE_GetAttributes"), FSFILE_GetAttributes_},
        {METHOD_NAME("FSFILE_GetSize"), FSFILE_GetSize_},
        {METHOD_NAME("FSFILE_Read"), FSFILE_Read_},
        {METHOD_NAME("FSDIR_Read"), FSDIR_Read_},
        {METHOD_NAME("FSDIR_Close"), FSDIR_Close_},
        
        {METHOD_NAME("System_IsAzaharInitialSetup"), System_IsAzaharInitialSetup},
        {METHOD_NAME("System_GetSystemFile"), System_GetSystemFile},
        {METHOD_NAME("System_GetNIM"), System_GetNIM},
    };

    bool obtainExheader() {
        Result loaderInitCustom(void);
        void loaderExitCustom(void);
        Result LOADER_GetLastApplicationProgramInfo(ExHeader_Info* exheaderInfo);


        Result res = loaderInitCustom();
        if (R_SUCCEEDED(res)) res = LOADER_GetLastApplicationProgramInfo(&lastAppExheader);
        loaderExitCustom();

        if (R_FAILED(res)) {
            logger.Error("Failed to get ExHeader. Luma3DS may be outdated.");
            return false;
        }

        return true;
    }

    static bool closeHandles() {
        auto CloseHandle = [](u64 handle, HandleType type) {
            switch (type)
            {
            case HandleType::FILE:
                logger.Debug("Call pending FSFILE_Close");
                FSFILE_Close((Handle)handle);
                break;
            case HandleType::DIR:
                logger.Debug("Call pending FSDIR_Close");
                FSDIR_Close((Handle)handle);
                break;
            case HandleType::ARCHIVE:
                logger.Debug("Call pending FSUSER_CloseArchive");
                FSUSER_CloseArchive((FS_Archive)handle);
                break;
            default:
                break;
            }
        };
        for (auto it = openHandles.begin(); it != openHandles.end(); it++) {
            CloseHandle(it->first, it->second);
        }
        openHandles.clear();
        return true;
    }

    std::vector<bool(*)()> setupFunctions {
        obtainExheader,
    };

    std::vector<bool(*)()> destructFunctions {
        closeHandles,
    };
}
