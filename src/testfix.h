/* Test-fixture input record/playback ("runbooks") for KEGS.
 * See testfix.c for the file format and usage. */

#ifndef KEGS_TESTFIX_H
#define KEGS_TESTFIX_H

void testfix_record_start(const char *fname);
void testfix_playback_start(const char *fname);
void testfix_poll(void);
int testfix_key_hook(int raw_a2code, unsigned int unicode_c, int is_up,
							int special);
void testfix_mouse_hook(int x, int y, int button_states, int buttons_valid);
void testfix_c025_hook(unsigned int new_c025_val, unsigned int mask);
void debug_testfix_record(const char *str);
void debug_testfix_play(const char *str);
void debug_testfix_stop(const char *str);
void debug_testfix_status(const char *str);

#endif
