/* Test-fixture input record/playback for KEGS - Playwright-style "runbooks".
 *
 * Records keyboard (key down/up) and mouse events with emulated-cycle
 * timestamps, and plays them back deterministically.  At the end of a
 * playback script the emulator halts into the debugger, so an attached
 * client (see debug_sock.c) can inspect machine state.
 *
 * Usage:
 *   kegs -record foo.kfix        record from power-on; F10 stops recording
 *   kegs -playback foo.kfix      play back from power-on; halt at script end
 *   Debugger commands (console or TCP socket):
 *       testfix record FILE / testfix play FILE / testfix stop / testfix
 *
 * F10 stops a recording (writing a final 'W' wait record so playback runs
 * to the same moment before halting) and aborts a playback.  F10 is used
 * instead of ESC so that ESC keypresses can themselves be recorded.
 *
 * File format (text, one event per line, '#' comments allowed):
 *   KEGSFIX1
 *   K <cycle> <raw_a2code hex> <unicode hex> <is_up>
 *   M <cycle> <x> <y> <button_states> <buttons_valid>
 *   C <cycle> <c025_val hex> <mask hex>
 *   W <cycle>
 * <cycle> is a decimal count of emulated 1MHz-ish cycles (g_cur_dfcyc>>16)
 * since the recording started.  'W' just waits until <cycle> passes.
 * 'C' carries modifier-key state ($c025: 1=shift, 2=ctrl, 4=capslock,
 * 0x40=option, 0x80=cmd/open-apple) - the Mac driver delivers modifiers
 * via flagsChanged->adb_update_c025_mask(), not as key up/down events.
 *
 * Timing: events are recorded at the ADB entry points (adb.c), which the
 * host drivers call between 60Hz VBLs, and are re-injected at the same
 * point in run_16ms(), so playback timing matches recording to within one
 * VBL of emulated time.  Emulated cycles do not advance while the debugger
 * or config panel is up, so playback pauses automatically at breakpoints.
 */

#include "defc.h"

#include "testfix.h"
#include "debug_sock.h"

#include <time.h>

extern dword64 g_cur_dfcyc;
extern Kimage g_mainwin_kimage;
extern int g_halt_sim;
extern int g_config_control_panel;

#define TESTFIX_OFF		0
#define TESTFIX_RECORD_ARM	1	/* waiting to latch base cycle */
#define TESTFIX_RECORD		2
#define TESTFIX_PLAY_ARM	3
#define TESTFIX_PLAY		4

#define TESTFIX_FNAME_SIZE	1024

static int	g_testfix_mode = TESTFIX_OFF;
static FILE	*g_testfix_file = 0;
static char	g_testfix_fname[TESTFIX_FNAME_SIZE];
static dword64	g_testfix_base_dfcyc = 0;
static long	g_testfix_num_events = 0;
static int	g_testfix_line_num = 0;

/* Next pending playback event, parsed ahead of time */
static int	g_testfix_pend_valid = 0;
static int	g_testfix_pend_type = 0;	/* 'K', 'M', 'C' or 'W' */
static dword64	g_testfix_pend_dfcyc_off = 0;
static int	g_testfix_pend_a2code = 0;
static word32	g_testfix_pend_unicode = 0;
static int	g_testfix_pend_is_up = 0;
static int	g_testfix_pend_x = 0;
static int	g_testfix_pend_y = 0;
static int	g_testfix_pend_buttons = 0;
static int	g_testfix_pend_buttons_valid = 0;
static word32	g_testfix_pend_c025_val = 0;
static word32	g_testfix_pend_c025_mask = 0;

static dword64
testfix_cycle_now(void)
{
	if(g_cur_dfcyc <= g_testfix_base_dfcyc) {
		return 0;
	}
	return (g_cur_dfcyc - g_testfix_base_dfcyc) >> 16;
}

static void
testfix_close_file(void)
{
	if(g_testfix_file) {
		fclose(g_testfix_file);
		g_testfix_file = 0;
	}
	g_testfix_pend_valid = 0;
}

const char *
testfix_mode_str(void)
{
	switch(g_testfix_mode) {
	case TESTFIX_RECORD_ARM:
	case TESTFIX_RECORD:
		return "recording";
	case TESTFIX_PLAY_ARM:
	case TESTFIX_PLAY:
		return "playback";
	}
	return "off";
}

static void
testfix_stop_any(const char *why)
{
	if(g_testfix_mode == TESTFIX_OFF) {
		return;
	}
	printf("testfix: stopping %s of %s (%s)\n", testfix_mode_str(),
						g_testfix_fname, why);
	testfix_close_file();
	g_testfix_mode = TESTFIX_OFF;
}

/* Parse the next K/M/W line into g_testfix_pend_*.  Returns 1 on success */
static int
testfix_parse_next(void)
{
	char	line[256];
	unsigned long long cycs;
	unsigned int a2code, unicode, c025_val, c025_mask;
	int	x, y, buttons, valid, is_up;

	g_testfix_pend_valid = 0;
	if(g_testfix_file == 0) {
		return 0;
	}
	while(fgets(line, sizeof(line), g_testfix_file) != 0) {
		g_testfix_line_num++;
		if((line[0] == '#') || (line[0] == '\n') || (line[0] == '\r') ||
						(line[0] == 0)) {
			continue;
		}
		if(strncmp(line, "KEGSFIX", 7) == 0) {
			continue;		/* header */
		}
		if(line[0] == 'K') {
			if(sscanf(line, "K %llu %x %x %d", &cycs, &a2code,
						&unicode, &is_up) == 4) {
				g_testfix_pend_type = 'K';
				g_testfix_pend_dfcyc_off =
						((dword64)cycs) << 16;
				g_testfix_pend_a2code = a2code & 0x7f;
				g_testfix_pend_unicode = unicode;
				g_testfix_pend_is_up = is_up;
				g_testfix_pend_valid = 1;
				return 1;
			}
		} else if(line[0] == 'M') {
			if(sscanf(line, "M %llu %d %d %d %d", &cycs, &x, &y,
						&buttons, &valid) == 5) {
				g_testfix_pend_type = 'M';
				g_testfix_pend_dfcyc_off =
						((dword64)cycs) << 16;
				g_testfix_pend_x = x;
				g_testfix_pend_y = y;
				g_testfix_pend_buttons = buttons;
				g_testfix_pend_buttons_valid = valid;
				g_testfix_pend_valid = 1;
				return 1;
			}
		} else if(line[0] == 'C') {
			if(sscanf(line, "C %llu %x %x", &cycs, &c025_val,
						&c025_mask) == 3) {
				g_testfix_pend_type = 'C';
				g_testfix_pend_dfcyc_off =
						((dword64)cycs) << 16;
				g_testfix_pend_c025_val = c025_val;
				g_testfix_pend_c025_mask = c025_mask;
				g_testfix_pend_valid = 1;
				return 1;
			}
		} else if(line[0] == 'W') {
			if(sscanf(line, "W %llu", &cycs) == 1) {
				g_testfix_pend_type = 'W';
				g_testfix_pend_dfcyc_off =
						((dword64)cycs) << 16;
				g_testfix_pend_valid = 1;
				return 1;
			}
		}
		printf("testfix: bad line %d in %s: %s", g_testfix_line_num,
						g_testfix_fname, line);
	}
	return 0;
}

void
testfix_record_start(const char *fname)
{
	time_t	now;

	testfix_stop_any("superseded");

	g_testfix_file = fopen(fname, "w");
	if(g_testfix_file == 0) {
		printf("testfix: cannot open %s for writing\n", fname);
		return;
	}
	snprintf(&g_testfix_fname[0], TESTFIX_FNAME_SIZE, "%s", fname);
	now = time(0);
	fprintf(g_testfix_file, "KEGSFIX1\n");
	fprintf(g_testfix_file, "# KEGS input runbook, recorded %s",
							ctime(&now));
	fprintf(g_testfix_file, "# K <cycle> <raw_a2code hex> <unicode hex> "
			"<is_up> | M <cycle> <x> <y> <buttons> "
			"<buttons_valid> | W <cycle>\n");
	fflush(g_testfix_file);
	g_testfix_num_events = 0;
	g_testfix_line_num = 0;
	g_testfix_mode = TESTFIX_RECORD_ARM;
	printf("testfix: recording input to %s, F10 to stop\n", fname);
}

void
testfix_playback_start(const char *fname)
{
	testfix_stop_any("superseded");

	g_testfix_file = fopen(fname, "r");
	if(g_testfix_file == 0) {
		printf("testfix: cannot open %s for reading\n", fname);
		return;
	}
	snprintf(&g_testfix_fname[0], TESTFIX_FNAME_SIZE, "%s", fname);
	g_testfix_num_events = 0;
	g_testfix_line_num = 0;
	if(!testfix_parse_next()) {
		printf("testfix: %s contains no events, ignoring\n", fname);
		testfix_close_file();
		return;
	}
	g_testfix_mode = TESTFIX_PLAY_ARM;
	printf("testfix: will play back %s, F10 aborts\n", fname);
}

static void
testfix_record_stop(void)
{
	fprintf(g_testfix_file, "W %llu\n",
			(unsigned long long)testfix_cycle_now());
	testfix_close_file();
	printf("testfix: recorded %ld events to %s\n", g_testfix_num_events,
						g_testfix_fname);
	debug_sock_write("testfix: recording stopped", 0);
	g_testfix_mode = TESTFIX_OFF;
}

static void
testfix_playback_finish(void)
{
	testfix_close_file();
	g_testfix_mode = TESTFIX_OFF;
	debug_sock_write("testfix: playback complete, halting", 0);
	/* halt2 so this works even without -noignhalt */
	halt2_printf("testfix: playback of %s complete, %ld events, "
		"cycle %llu\n", g_testfix_fname, g_testfix_num_events,
		(unsigned long long)testfix_cycle_now());
}

/* Called from adb_physical_key_update() for main-window keys, after the
 *  special-key mapping but before any key processing.  Returns 1 if the
 *  key was consumed (F10 stopping a recording/playback) */
int
testfix_key_hook(int raw_a2code, word32 unicode_c, int is_up, int special)
{
	if(g_testfix_mode == TESTFIX_OFF) {
		return 0;
	}
	if((special == 0x0a) && !is_up) {		/* F10 */
		switch(g_testfix_mode) {
		case TESTFIX_RECORD:
			testfix_record_stop();
			return 1;
		case TESTFIX_RECORD_ARM:
		case TESTFIX_PLAY_ARM:
		case TESTFIX_PLAY:
			testfix_stop_any("F10 pressed");
			return 1;
		}
	}
	if(g_testfix_mode == TESTFIX_RECORD) {
		fprintf(g_testfix_file, "K %llu %02x %04x %d\n",
			(unsigned long long)testfix_cycle_now(),
			raw_a2code & 0x7f, unicode_c, is_up != 0);
		fflush(g_testfix_file);
		g_testfix_num_events++;
	}
	return 0;
}

/* Called from adb_update_c025_mask() for main-window modifier-key state
 *  changes (shift/ctrl/capslock/option/cmd, i.e. open-apple).  The Mac
 *  driver delivers modifiers this way instead of as key up/down events */
void
testfix_c025_hook(word32 new_c025_val, word32 mask)
{
	if(g_testfix_mode != TESTFIX_RECORD) {
		return;
	}
	fprintf(g_testfix_file, "C %llu %02x %02x\n",
		(unsigned long long)testfix_cycle_now(), new_c025_val, mask);
	fflush(g_testfix_file);
	g_testfix_num_events++;
}

/* Called from adb_update_mouse() for main-window mouse events */
void
testfix_mouse_hook(int x, int y, int button_states, int buttons_valid)
{
	if(g_testfix_mode != TESTFIX_RECORD) {
		return;
	}
	fprintf(g_testfix_file, "M %llu %d %d %d %d\n",
		(unsigned long long)testfix_cycle_now(), x, y, button_states,
		buttons_valid);
	fflush(g_testfix_file);
	g_testfix_num_events++;
}

/* Called once per run_16ms().  Latches the timing base once emulation is
 *  actually running, and injects any playback events that are due */
void
testfix_poll(void)
{
	dword64	target;

	if(g_testfix_mode == TESTFIX_OFF) {
		return;
	}
	if(g_halt_sim || g_config_control_panel) {
		return;		/* emulated time is frozen, nothing to do */
	}
	if(g_testfix_mode == TESTFIX_RECORD_ARM) {
		g_testfix_base_dfcyc = g_cur_dfcyc;
		g_testfix_mode = TESTFIX_RECORD;
		return;
	}
	if(g_testfix_mode == TESTFIX_PLAY_ARM) {
		g_testfix_base_dfcyc = g_cur_dfcyc;
		g_testfix_mode = TESTFIX_PLAY;
	}
	if(g_testfix_mode != TESTFIX_PLAY) {
		return;
	}
	while(g_testfix_pend_valid) {
		target = g_testfix_base_dfcyc + g_testfix_pend_dfcyc_off;
		if(g_cur_dfcyc < target) {
			return;			/* not due yet */
		}
		switch(g_testfix_pend_type) {
		case 'K':
			adb_physical_key_update(&g_mainwin_kimage,
				g_testfix_pend_a2code, g_testfix_pend_unicode,
				g_testfix_pend_is_up);
			break;
		case 'M':
			adb_update_mouse(&g_mainwin_kimage, g_testfix_pend_x,
				g_testfix_pend_y, g_testfix_pend_buttons,
				g_testfix_pend_buttons_valid);
			break;
		case 'C':
			adb_update_c025_mask(&g_mainwin_kimage,
				g_testfix_pend_c025_val,
				g_testfix_pend_c025_mask);
			break;
		case 'W':
			break;			/* just waits until target */
		}
		g_testfix_num_events++;
		testfix_parse_next();
	}
	testfix_playback_finish();
}

/* Debugger commands: testfix record FILE, testfix play FILE,
 *  testfix stop, testfix (status) */

static const char *
testfix_trim_arg(const char *str, char *buf, int buf_size)
{
	int	len;

	while((*str == ' ') || (*str == '\t')) {
		str++;
	}
	snprintf(buf, buf_size, "%s", str);
	len = (int)strlen(buf);
	while((len > 0) && ((buf[len-1] == '\n') || (buf[len-1] == '\r') ||
			(buf[len-1] == ' ') || (buf[len-1] == '\t'))) {
		buf[--len] = 0;
	}
	return buf;
}

void
debug_testfix_record(const char *str)
{
	char	buf[TESTFIX_FNAME_SIZE];

	testfix_trim_arg(str, &buf[0], TESTFIX_FNAME_SIZE);
	if(buf[0] == 0) {
		dbg_printf("Usage: testfix record FILE\n");
		return;
	}
	testfix_record_start(&buf[0]);
	dbg_printf("testfix: recording to %s armed, resume with 'g', F10 "
							"stops\n", &buf[0]);
}

void
debug_testfix_play(const char *str)
{
	char	buf[TESTFIX_FNAME_SIZE];

	testfix_trim_arg(str, &buf[0], TESTFIX_FNAME_SIZE);
	if(buf[0] == 0) {
		dbg_printf("Usage: testfix play FILE\n");
		return;
	}
	testfix_playback_start(&buf[0]);
	if(g_testfix_mode == TESTFIX_PLAY_ARM) {
		dbg_printf("testfix: playback of %s armed, resume with 'g'\n",
								&buf[0]);
	}
}

void
debug_testfix_stop(const char *str)
{
	if(g_testfix_mode == TESTFIX_RECORD) {
		testfix_record_stop();
	} else {
		testfix_stop_any("stop command");
	}
	dbg_printf("testfix: now off\n");
}

void
debug_testfix_status(const char *str)
{
	dbg_printf("testfix: %s", testfix_mode_str());
	if(g_testfix_mode != TESTFIX_OFF) {
		dbg_printf(" %s, %ld events, cycle %llu", g_testfix_fname,
			g_testfix_num_events,
			(unsigned long long)testfix_cycle_now());
	}
	dbg_printf("\n");
}
