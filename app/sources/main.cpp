
#include <stdio.h>
#include <stdlib.h>
#include "sys/stat.h"
#include <string.h>
#include <memory.h>

#include "Main.hpp"
#include "plgldr.h"
#include "BCLIM.hpp"
#include "logo.h"
#include "plugin.h"
#include "3gx.h"

Logger logger;

const char* artic_setup_plugin = "/3ds/AzaharArticSetup/AzaharArticSetup.3gx";

char *strdup(const char *s) {
	char *d = (char*)malloc(strlen(s) + 1);
	if (d == NULL) return NULL;
	strcpy(d, s);
	return d;
}

FILE* fopen_mkdir(const char* name, const char* mode, bool actuallyOpen = true)
{
	char*	_path = strdup(name);
	char    *p;
	FILE*	retfile = NULL;

	errno = 0;
	for (p = _path + 1; *p; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			if (mkdir(_path, 777) != 0)
				if (errno != EEXIST) goto error;
			*p = '/';
		}
	}
	if (actuallyOpen) retfile = fopen(name, mode);
error:
	free(_path);
	return retfile;
}

bool extractPlugin() {
    u32 expectedVersion = SYSTEM_VERSION(VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    bool plugin_needs_update = true;
    FILE* f = fopen(artic_setup_plugin, "r");
    if (f) {
        _3gx_Header header;
        int read = fread(&header, 1, sizeof(header), f);
        if (read == sizeof(header) && header.magic == _3GX_MAGIC) {
            plugin_needs_update = header.version != expectedVersion;
        }
    }
    if (f) fclose(f);

    if (plugin_needs_update) {
        logger.Info("Updating Azahar Artic Setup plugin file");
        f = fopen_mkdir(artic_setup_plugin, "w");
        if (!f) {
            logger.Error("Cannot open plugin file");
            return false;
        }
        int written = fwrite(plugin_AzaharArticSetup_3gx, 1, plugin_AzaharArticSetup_3gx_size, f);
        fclose(f);
        if (written != plugin_AzaharArticSetup_3gx_size) {
            logger.Error("Cannot write plugin file");
            return false;
        }
    }
    return true;
}

bool launchPlugin() {
	u32 ret = 0;
	PluginLoadParameters plgparam = { 0 };
	u8 isPlgEnabled = 0;

	plgparam.noFlash = true;
	plgparam.pluginMemoryStrategy = PLG_STRATEGY_NONE;
    plgparam.persistent = 0;
	plgparam.lowTitleId = 0;
    strcpy(plgparam.path, artic_setup_plugin);

	ret = plgLdrInit();
	if (R_FAILED(ret)) {
        logger.Error("Cannot start plugin loader");
        return false;
    }
    u32 version;
    ret = PLGLDR__GetVersion(&version);
    if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        plgLdrExit();
        return false;
    }
    if (version < SYSTEM_VERSION(1,0,2)) {
        logger.Error("Unsupported plugin loader version,");
        logger.Error("please update Luma3DS");
        plgLdrExit();
        return false;
    }
	ret = PLGLDR__IsPluginLoaderEnabled((bool*)&isPlgEnabled);
    if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        plgLdrExit();
        return false;
    }
	plgparam.config[0] = isPlgEnabled;
	ret = PLGLDR__SetPluginLoaderState(true);
	if (R_FAILED(ret)) {
        logger.Error("Cannot enable plugin loader");
        plgLdrExit();
        return false;
    }
	ret = PLGLDR__SetPluginLoadParameters(&plgparam);
    plgLdrExit();
	if (R_FAILED(ret)) {
        logger.Error("Plugin loader error");
        return false;
    }
    return true;
}

bool checkEmulator() {
    s64 out = 0;
	svcGetSystemInfo(&out, 0x20000, 0);
	return out != 0;
}

PrintConsole topScreenConsole, bottomScreenConsole;
int transferedBytes = 0;
void Main() {
    logger.Start();
    logger.debug_enable = true;

    gfxInitDefault();
	consoleInit(GFX_TOP, &topScreenConsole);
    consoleInit(GFX_BOTTOM, &bottomScreenConsole);
    topScreenConsole.bg = 15; topScreenConsole.fg = 0;
    bottomScreenConsole.bg = 15; bottomScreenConsole.fg = 0;

    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    aptSetHomeAllowed(false);

    consoleSelect(&bottomScreenConsole);
    consoleClear();
    consoleSelect(&topScreenConsole);
    consoleClear();

    bool isEmulator = checkEmulator();

    {
        CTRPluginFramework::BCLIM((void*)__data_logo_bin, __data_logo_bin_size).Render(CTRPluginFramework::Rect<int>((320 - 128) / 2, (240 - 128) / 2, 128, 128));
    }
    logger.Raw(false, "\n      Azahar Artic Setup v%d.%d.%d\n", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
    logger.Raw(false, isEmulator ? " " : "    Press A to launch setup tool.");
    logger.Raw(false, "    Press B or START to exit.");
    logger.Raw(true, "");
    logger.Info("Welcome to Azahar Artic Setup Tool!\n    Only use this tool with Azahar Emulator\n\n    Check bottom screen for controls.");
    
    if (isEmulator) {
        logger.Error("This tool can only be used on a real console.");
    }

    bool do_jump = false;
    while (aptMainLoop())
	{
		//Scan all the inputs. This should be done once for each frame
		hidScanInput();

		//hidKeysDown returns information about which buttons have been just pressed (and they weren't in the previous frame)
		u32 kDown = hidKeysDown();

        if (kDown & (KEY_B | KEY_START)) {
            break;
        }

        if ((kDown & KEY_A) && !isEmulator) {
            logger.Info("Launching Azahar Artic Setup");
            bool done = extractPlugin() && launchPlugin();
            if (done) {
                do_jump = true;
                logger.Raw(true, "");
                logger.Info("Done! Please wait...");
                svcSleepThread(3000000000);
                break;
            } else {
                logger.Error("Failed to launch Azahar Artic Setup");
            }
        }

		// Flush and swap framebuffers
		gfxFlushBuffers();
		gfxSwapBuffers();

		//Wait for VBlank
		gspWaitForVBlank();
	}

    // Flush and swap framebuffers
    gfxFlushBuffers();
    gfxSwapBuffers();

    //Wait for VBlank
    gspWaitForVBlank();

	gfxExit();
    gspLcdExit();
    logger.End();

    if (do_jump) {
        OS_VersionBin cver = {0};
        OS_VersionBin nver = {0};
        osGetSystemVersionData(&nver, &cver);
        u64 program_id = 0;
        if (cver.region == 'E') {
            program_id = 0x0004001000022000;
        } else if (cver.region == 'U') {
            program_id = 0x0004001000021000;
        } else if (cver.region == 'J') {
            program_id = 0x0004001000020000;
        } else if (cver.region == 'C') {
            program_id = 0x0004001000026000;
        } else if (cver.region == 'K') {
            program_id = 0x0004001000027000;
        } else if (cver.region == 'T') {
            program_id = 0x0004001000028000;
        }
        aptSetChainloader(program_id, MEDIATYPE_NAND);
    }
}

extern "C" {
	int main(int argc, char* argv[]);
}
// Entrypoint, game will starts when you exit this function
int    main(int argc, char* argv[])
{
	Main();
	return 0;
}
