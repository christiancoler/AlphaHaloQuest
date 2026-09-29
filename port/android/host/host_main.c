/*
HOST_MAIN.C

Entry point of the Android port (SDL_main, called by SDLActivity on its
own thread).

It loads the guest image (the game, built as ILP32 code) from the APK's
assets, gives it an environment describing where the game data and saves
live, and runs its main() on a thread of its own with its stack in guest
memory (host_thread.c), on which everything here after startup runs; the
SDL thread waits for it.

Storage (see port/android/README.md): the game data (the directory holding
maps/) is the app's external files directory,
/sdcard/Android/data/<package>/files, where the launcher activity copies it
on first run; saves go to its save/ subdirectory. The settings,
config.toml, live there too (port/linux/src/port_config.c, which the game
reads); this file reads only debug.sample_seconds from it, for the sampler
that runs here.
*/

#include "host.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

void host_install_signal_handlers(void);

/* ---------- logging and termination */

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	__android_log_vprint(priority, "halo", format, arguments);
	va_end(arguments);
}

void host_log(int priority, const char *text)
{
	__android_log_write(priority, "halo", text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	__android_log_write(ANDROID_LOG_FATAL, "halo", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	__android_log_print(ANDROID_LOG_FATAL, "halo", "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	/* the process ends with the game; Android restarts it from the
	launcher next time */
	_exit(code);
}

int host_errno(void)
{
	return errno;
}

/* ---------- paths */

static char data_root[512];
static char save_root[512];

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

static int directory_has_maps(const char *root)
{
	char path[600];
	struct stat information;

	snprintf(path, sizeof(path), "%s/maps/ui.map", root);
	return stat(path, &information) == 0;
}

/* Directories the app creates in its external storage are private to it
(mode 0770 under the app's own group), so the shell user (adb) cannot list
them. Open the save tree for reading, with set-group-ID directories as
posix_make_directory creates them (port/linux/src/posix_files.c). */
static int share_entry(const char *path, const struct stat *information, int type, struct FTW *walk)
{
	(void)information;
	(void)walk;
	if (type == FTW_D || type == FTW_DP)
		chmod(path, 02775);
	else if (type == FTW_F)
		chmod(path, 0664);
	return 0;
}

static void share_save_tree(const char *root)
{
	nftw(root, share_entry, 16, FTW_PHYS);
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;
	int index;

	entry = malloc(length + strlen(value) + 2);
	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

/* debug.sample_seconds from config.toml, as text for the sampler, or 0 */
static int config_sample_seconds(const char *path, char *text, size_t size)
{
	toml_result_t result = toml_parse_file_ex(path);
	int found = 0;

	if (!result.ok)
		return 0;
	{
		toml_datum_t seconds = toml_seek(result.toptab, "debug.sample_seconds");
		double value = seconds.type == TOML_FP64 ? seconds.u.fp64 :
			seconds.type == TOML_INT64 ? (double)seconds.u.int64 : 0.0;

		if (value > 0.0)
		{
			snprintf(text, size, "%g", value);
			found = 1;
		}
	}
	toml_free(result);
	return found;
}

static int config_uses_automatic_render_scale(const char *path)
{
	toml_result_t result = toml_parse_file_ex(path);
	int automatic = 1;
	if (!result.ok)
		return 1;
	{
		toml_datum_t setting = toml_seek(result.toptab, "vr.auto_render_scale");
		if (setting.type == TOML_BOOLEAN)
			automatic = setting.u.boolean;
		else
		{
			/* Keep an old 2x performance override. The old 3x default is
			   migrated to the new device-specific automatic value. */
			toml_datum_t scale = toml_seek(result.toptab, "vr.render_scale");
			double value = scale.type == TOML_FP64 ? scale.u.fp64 :
				scale.type == TOML_INT64 ? (double)scale.u.int64 : 3.0;
			automatic = value >= 3.0;
		}
	}
	toml_free(result);
	return automatic;
}

/* Leave headroom for the texture-allocation spike during campaign loading.
   Quest 3/3S/Pro can still use a larger source image. The config file remains
   authoritative, so a manual scale can go higher. */
static float quest_default_render_scale(void)
{
	char device[PROP_VALUE_MAX] = { 0 };
	char model[PROP_VALUE_MAX] = { 0 };
	__system_property_get("ro.product.device", device);
	__system_property_get("ro.product.model", model);
	if (strstr(device, "eureka") || strstr(device, "panther") ||
		strstr(device, "quest3") || strstr(model, "Quest 3") ||
		strstr(model, "Quest Pro"))
		return 4.0f;
	/* 1200 square pixels per eye is a better Quest 2 balance. The compositor
	   still outputs at the panel resolution, while the GPU has about 30% fewer
	   source pixels to shade than at the previous 1440-square setting. */
	return 2.5f;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* copies argv and the environment into guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, PROT_READ | PROT_WRITE);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = (uint32_t)getpagesize();
	return (uint32_t)(uintptr_t)boot;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *game_main(void *unused)
{
	struct environment environment = { { 0 }, 0 };
	const char *external;
	char zone[64];
	char path[600];
	size_t image_size = 0;
	void *image;
	uint32_t boot;

	(void)unused;
	external = SDL_GetAndroidExternalStoragePath();
	if (!external)
		host_fatal("Android storage is unavailable: %s", SDL_GetError());
	snprintf(data_root, sizeof(data_root), "%s", external);
	snprintf(save_root, sizeof(save_root), "%s/save", external);
	/* readable by adb (the shell user), for managing saves */
	mkdir(save_root, 0775);
	share_save_tree(save_root);
	if (!directory_has_maps(data_root))
	{
		host_fatal("The original-Xbox Halo CE game data was not found.\n\nCopy the folder that "
			"contains maps into\n%s\nor import an Xbox disc image from the launcher screen.", data_root);
	}

	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);
	snprintf(path, sizeof(path), "%s/config.toml", data_root);
	{
		/* the game renders 480 lines at the display's aspect ratio
		(landscape) unless display.screen_width says otherwise (d3d8_gl.c) */
		const SDL_DisplayMode *mode;
		char width[16];
		char render_scale[16];

		SDL_InitSubSystem(SDL_INIT_VIDEO);
		mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
		if (mode && mode->w > 0 && mode->h > 0)
		{
			/* Two square logical eye images. d3d8_gl applies vr.render_scale to
			the physical backbuffer before OpenXR copies it into its swapchains. */
			snprintf(width, sizeof(width), "%d", 960);
			environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
			environment_set(&environment, "HALO_QUEST_VR", "1");
			if (config_uses_automatic_render_scale(path))
			{
				snprintf(render_scale, sizeof(render_scale), "%g", quest_default_render_scale());
				environment_set(&environment, "HALO_VR_RENDER_SCALE", render_scale);
				host_logf(HOST_LOG_INFO, "automatic Quest render scale %s", render_scale);
			}
			host_logf(HOST_LOG_INFO, "display %dx%d: Quest VR rendering %sx480", mode->w, mode->h, width);
		}
	}
	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);

	image = SDL_LoadFile("halo_guest.elf", &image_size);
	if (!image)
		host_fatal("cannot read the game image from the APK: %s", SDL_GetError());
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image; see logcat (tag \"halo\") for details");
	SDL_free(image);

	{
		char seconds[32];

		if (config_sample_seconds(path, seconds, sizeof(seconds)))
			host_debug_start_sampler(seconds);
	}
	boot = make_boot(&environment);
	host_logf(HOST_LOG_INFO, "data %s, saves %s", data_root, save_root);
	host_run_guest_main(boot);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	host_logf(HOST_LOG_INFO, "Halo for Android starting");
	host_install_signal_handlers();
	if (host_native_thread_create(game_main, NULL, MAIN_STACK_SIZE) != 0)
		host_fatal("cannot start the game thread");
	/* the game ends the process itself (host_exit) */
	for (;;)
		pause();
}
