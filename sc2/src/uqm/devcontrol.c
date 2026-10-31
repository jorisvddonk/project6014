/*
 * Direct control socket (dev aid).
 *
 * Development aid: lets an external process drive the game without OS input
 * automation (macOS blocks synthetic CGEvents for unsigned binaries). A line
 * based protocol over a unix domain socket injects normal SDL key events into
 * the game's own event queue, so the entire input path is exercised.
 *
 * Enabled with P6014_CONTROL=1; socket path from P6014_CONTROL_SOCKET
 * (default /tmp/p6014-input.sock).
 *
 * Commands (one per line):
 *   key <name>     press+release (name = Return, Escape, Right, Down, a, 1, ...)
 *   down <name>    press and hold
 *   up <name>      release
 *   keycode <n>    press+release by SDLK value
 *   sleep <ms>     pause in the server thread (keeps ordering)
 *   quit           post SDL_QUIT
 */

#include "libs/log.h"
#include "port.h"

#include SDL_INCLUDE(SDL.h)

#define control_log(...) log_add (log_User, __VA_ARGS__)

/* p6014 builds against SDL 1.2 (SDLKey); newer builds use SDL 2 keycodes. */
#if SDL_VERSION_ATLEAST(2, 0, 0)
typedef control_key control_key;
#	define CONTROL_SCANCODE(sym) SDL_GetScancodeFromKey (sym)
#else
typedef SDLKey control_key;
#	define CONTROL_SCANCODE(sym) 0
#endif

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void
p6014_control_push (Uint32 type, control_key sym)
{
	SDL_Event ev;

	memset (&ev, 0, sizeof ev);
	ev.type = type;
	ev.key.type = type;
	ev.key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
	ev.key.keysym.sym = sym;
	ev.key.keysym.scancode = CONTROL_SCANCODE (sym);
	{
		int rc = SDL_PushEvent (&ev);

		if (rc < 0)
			control_log ("control: SDL_PushEvent(%d) failed: %s",
					(int) sym, SDL_GetError ());
	}
}

static control_key
p6014_control_lookup (const char *name)
{
	static const struct { const char *name; control_key sym; } keys[] = {
		{ "Return", SDLK_RETURN }, { "Enter", SDLK_RETURN },
		{ "Escape", SDLK_ESCAPE }, { "Esc", SDLK_ESCAPE },
		{ "Space", SDLK_SPACE }, { "Tab", SDLK_TAB },
		{ "Backspace", SDLK_BACKSPACE }, { "Delete", SDLK_DELETE },
		{ "Left", SDLK_LEFT }, { "Right", SDLK_RIGHT },
		{ "Up", SDLK_UP }, { "Down", SDLK_DOWN },
		{ "Home", SDLK_HOME }, { "End", SDLK_END },
		{ "PageUp", SDLK_PAGEUP }, { "PageDown", SDLK_PAGEDOWN },
		{ "F1", SDLK_F1 }, { "F2", SDLK_F2 }, { "F3", SDLK_F3 },
		{ "F4", SDLK_F4 }, { "F5", SDLK_F5 }, { "F6", SDLK_F6 },
		{ "F7", SDLK_F7 }, { "F8", SDLK_F8 }, { "F9", SDLK_F9 },
		{ "F10", SDLK_F10 }, { "F11", SDLK_F11 }, { "F12", SDLK_F12 },
	};
	size_t i;

	for (i = 0; i < sizeof keys / sizeof keys[0]; ++i)
		if (strcasecmp (keys[i].name, name) == 0)
			return keys[i].sym;

	if (strlen (name) == 1)
	{
		char c = name[0];

		if (c >= 'a' && c <= 'z')
			return SDLK_a + (c - 'a');
		if (c >= 'A' && c <= 'Z')
			return SDLK_a + (c - 'A');
		if (c >= '0' && c <= '9')
			return SDLK_0 + (c - '0');
		if (c == ' ')
			return SDLK_SPACE;
	}

	return (control_key) strtol (name, NULL, 0);
}

static void
p6014_control_handle (char *line)
{
	char cmd[32] = "";
	char arg[64] = "";
	int n = sscanf (line, "%31s %63s", cmd, arg);

	if (n < 1)
		return;
	if (strcasecmp (cmd, "quit") == 0)
	{
		SDL_Event ev;

		memset (&ev, 0, sizeof ev);
		ev.type = SDL_QUIT;
		SDL_PushEvent (&ev);
	}
	else if (strcasecmp (cmd, "sleep") == 0)
		usleep ((useconds_t) (atol (arg) * 1000));
	else if (strcasecmp (cmd, "shot") == 0 && n >= 2)
	{	// save the current video surface (works even with the screen locked)
		SDL_Surface *surface = SDL_GetVideoSurface ();

		if (surface != NULL)
			control_log ("control: shot %s -> %d", arg,
					SDL_SaveBMP (surface, arg));
		else
			control_log ("control: shot %s -> no surface", arg);
	}
	else if (n >= 2)
	{
		control_key sym = p6014_control_lookup (arg);

		if (strcasecmp (cmd, "key") == 0 || strcasecmp (cmd, "keycode") == 0)
		{
			p6014_control_push (SDL_KEYDOWN, sym);
			usleep (150000);
			p6014_control_push (SDL_KEYUP, sym);
		}
		else if (strcasecmp (cmd, "down") == 0)
			p6014_control_push (SDL_KEYDOWN, sym);
		else if (strcasecmp (cmd, "up") == 0)
			p6014_control_push (SDL_KEYUP, sym);
	}
}

static void *
p6014_control_thread (void *arg)
{
	const char *path = arg;
	int server;
	struct sockaddr_un addr;

	server = socket (AF_UNIX, SOCK_STREAM, 0);
	if (server < 0)
	{
		control_log ("control: socket() failed: %s", strerror (errno));
		return NULL;
	}

	memset (&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy (addr.sun_path, path, sizeof addr.sun_path - 1);
	unlink (path);
	if (bind (server, (struct sockaddr *) &addr, sizeof addr) < 0
			|| listen (server, 4) < 0)
	{
		control_log ("control: bind/listen failed: %s", strerror (errno));
		close (server);
		return NULL;
	}
	control_log ("control: listening on %s", path);

	for (;;)
	{
		int client = accept (server, NULL, NULL);
		FILE *fp;
		char line[256];

		if (client < 0)
			continue;
		fp = fdopen (client, "r");
		if (!fp)
		{
			close (client);
			continue;
		}
		while (fgets (line, sizeof line, fp))
		{
			line[strcspn (line, "\r\n")] = '\0';
			control_log ("control: %s", line);
			p6014_control_handle (line);
		}
		fclose (fp);
		/* keep going: several clients may connect over a session */
	}

	return NULL;
}

void
p6014_control_init (void)
{
	const char *path;
	pthread_t thread;

	if (!getenv ("P6014_CONTROL"))
		return;
	path = getenv ("P6014_CONTROL_SOCKET");
	if (!path || !*path)
		path = "/tmp/p6014-input.sock";
	pthread_create (&thread, NULL, p6014_control_thread, (void *) path);
	pthread_detach (thread);
}
