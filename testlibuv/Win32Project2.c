// Win32Project2.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include "Win32Project2.h"

#include "defs.h"

#define DEFAULT_BIND_HOST     "127.0.0.1"
#define DEFAULT_BIND_PORT     1080
#define DEFAULT_IDLE_TIMEOUT  (60 * 1000)

static char *modulename = 0;
static const char *progname = 0;//__FILE__;  /* Reset in main(). */

const char *_getprogname(void) {
	return progname;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
	server_config config;

	modulename = malloc(MAX_PATH);
	int nLen = GetModuleFileNameA(hInstance, modulename, MAX_PATH);
	if (nLen <= 0) return 0;

	progname = strrchr(modulename, '\\');
	if (0 != progname) progname++;
	
	memset(&config, 0, sizeof(config));
	config.bind_host = DEFAULT_BIND_HOST;
	config.bind_port = DEFAULT_BIND_PORT;
	config.idle_timeout = DEFAULT_IDLE_TIMEOUT;

	int err = server_run(&config, uv_default_loop());
	if (err) {

		exit(1);

	}

	return 0;
}
