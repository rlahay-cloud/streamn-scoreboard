#include "scoreboard-core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#define getpid() _getpid()
#else
#include <unistd.h>
#endif

static char g_tmp_dir[256];

static void setup_tmp_dir(void)
{
#ifdef _WIN32
	snprintf(g_tmp_dir, sizeof(g_tmp_dir), "%s\\scoreboard_pm_test_%d",
		 getenv("TEMP") ? getenv("TEMP") : ".", (int)getpid());
#else
	snprintf(g_tmp_dir, sizeof(g_tmp_dir), "/tmp/scoreboard_pm_test_%d",
		 (int)getpid());
#endif
	mkdir(g_tmp_dir, 0755);
}

static void cleanup_tmp_dir(void)
{
	char cmd[512];
#ifdef _WIN32
	snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", g_tmp_dir);
#else
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp_dir);
#endif
	system(cmd);
}

static char *read_file_content(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (char *)malloc((size_t)size + 1);
	size_t n = fread(buf, 1, (size_t)size, f);
	buf[n] = '\0';
	fclose(f);
	return buf;
}

static void expect_file(const char *name, const char *expected)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", g_tmp_dir, name);
	char *content = read_file_content(path);
	assert(content != NULL);
	assert(strcmp(content, expected) == 0);
	free(content);
}

/* Home roster 10, 11, 12 and away roster 20, 21, 22. */
static void setup_rosters(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(true, 10);
	scoreboard_roster_add(true, 11);
	scoreboard_roster_add(true, 12);
	scoreboard_roster_add(false, 20);
	scoreboard_roster_add(false, 21);
	scoreboard_roster_add(false, 22);
}

/* ---- roster ---- */

static void test_roster_add_and_get(void)
{
	scoreboard_reset_state_for_tests();
	assert(scoreboard_roster_count(true) == 0);
	assert(scoreboard_roster_count(false) == 0);

	assert(scoreboard_roster_add(true, 17) == 0);
	assert(scoreboard_roster_add(true, 4) == 1);
	assert(scoreboard_roster_add(false, 99) == 0);
	assert(scoreboard_roster_count(true) == 2);
	assert(scoreboard_roster_count(false) == 1);

	const struct scoreboard_player *p = scoreboard_roster_get(true, 0);
	assert(p != NULL);
	assert(p->number == 17);
	assert(!p->on_ice);
	assert(p->plus_minus == 0);
	assert(scoreboard_roster_get(true, 1)->number == 4);
	assert(scoreboard_roster_get(false, 0)->number == 99);

	assert(scoreboard_roster_find(true, 17));
	assert(!scoreboard_roster_find(true, 99));
	assert(scoreboard_roster_find(false, 99));
}

static void test_roster_add_duplicate_returns_existing(void)
{
	scoreboard_reset_state_for_tests();
	assert(scoreboard_roster_add(true, 8) == 0);
	assert(scoreboard_roster_add(true, 9) == 1);
	assert(scoreboard_roster_add(true, 8) == 0);
	assert(scoreboard_roster_count(true) == 2);
}

static void test_roster_add_rejects_bad_numbers(void)
{
	scoreboard_reset_state_for_tests();
	assert(scoreboard_roster_add(true, -1) == -1);
	assert(scoreboard_roster_add(true, SCOREBOARD_MAX_PLAYER_NUMBER + 1) ==
	       -1);
	assert(scoreboard_roster_add(true, 0) == 0);
	assert(scoreboard_roster_add(true, SCOREBOARD_MAX_PLAYER_NUMBER) == 1);
	assert(scoreboard_roster_count(true) == 2);
}

static void test_roster_full(void)
{
	scoreboard_reset_state_for_tests();
	for (int i = 0; i < SCOREBOARD_MAX_ROSTER; i++)
		assert(scoreboard_roster_add(true, i + 1) == i);
	assert(scoreboard_roster_add(true, 500) == -1);
	assert(scoreboard_roster_count(true) == SCOREBOARD_MAX_ROSTER);
	/* The other team has its own roster. */
	assert(scoreboard_roster_add(false, 500) == 0);
}

static void test_roster_get_out_of_range(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(true, 5);
	assert(scoreboard_roster_get(true, -1) == NULL);
	assert(scoreboard_roster_get(true, 1) == NULL);
	assert(scoreboard_roster_get(false, 0) == NULL);
}

static void test_roster_remove_keeps_order(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(true, 1);
	scoreboard_roster_add(true, 2);
	scoreboard_roster_add(true, 3);
	assert(scoreboard_roster_remove(true, 2));
	assert(scoreboard_roster_count(true) == 2);
	assert(scoreboard_roster_get(true, 0)->number == 1);
	assert(scoreboard_roster_get(true, 1)->number == 3);
	/* Removing the last entry works too. */
	assert(scoreboard_roster_remove(true, 3));
	assert(scoreboard_roster_count(true) == 1);
	assert(!scoreboard_roster_remove(true, 3));
	assert(!scoreboard_roster_remove(false, 1));
}

static void test_roster_clear(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(true, 1);
	scoreboard_roster_add(false, 2);
	scoreboard_roster_clear(true);
	assert(scoreboard_roster_count(true) == 0);
	assert(scoreboard_roster_count(false) == 1);
}

static void test_roster_marks_dirty(void)
{
	scoreboard_reset_state_for_tests();
	assert(!scoreboard_is_dirty());
	scoreboard_roster_add(true, 7);
	assert(scoreboard_is_dirty());
}

/* ---- on ice ---- */

static void test_on_ice_set_toggle_clear(void)
{
	setup_rosters();
	assert(scoreboard_roster_on_ice_count(true) == 0);

	assert(scoreboard_player_set_on_ice(true, 10, true));
	assert(scoreboard_player_set_on_ice(true, 11, true));
	assert(scoreboard_roster_on_ice_count(true) == 2);
	assert(scoreboard_roster_on_ice_count(false) == 0);
	assert(scoreboard_roster_get(true, 0)->on_ice);

	assert(scoreboard_player_toggle_on_ice(true, 10));
	assert(!scoreboard_roster_get(true, 0)->on_ice);
	assert(scoreboard_player_toggle_on_ice(true, 12));
	assert(scoreboard_roster_on_ice_count(true) == 2);

	assert(scoreboard_player_set_on_ice(true, 11, false));
	assert(scoreboard_roster_on_ice_count(true) == 1);

	scoreboard_roster_clear_on_ice(true);
	assert(scoreboard_roster_on_ice_count(true) == 0);
}

static void test_on_ice_unknown_player(void)
{
	setup_rosters();
	assert(!scoreboard_player_set_on_ice(true, 99, true));
	assert(!scoreboard_player_toggle_on_ice(true, 99));
	/* 20 is an away player, not a home one. */
	assert(!scoreboard_player_set_on_ice(true, 20, true));
	assert(scoreboard_roster_on_ice_count(true) == 0);
}

/* ---- goals ---- */

static void test_home_goal_credits_on_ice_players(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(true, 11, true);
	scoreboard_player_set_on_ice(false, 20, true);

	scoreboard_increment_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(true, 11) == 1);
	assert(scoreboard_player_get_plus_minus(true, 12) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
	assert(scoreboard_player_get_plus_minus(false, 21) == 0);
}

static void test_away_goal_credits_on_ice_players(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_player_set_on_ice(false, 21, true);

	scoreboard_increment_away_score();

	assert(scoreboard_player_get_plus_minus(false, 20) == 1);
	assert(scoreboard_player_get_plus_minus(false, 21) == 1);
	assert(scoreboard_player_get_plus_minus(false, 22) == 0);
	assert(scoreboard_player_get_plus_minus(true, 10) == -1);
	assert(scoreboard_player_get_plus_minus(true, 11) == 0);
}

static void test_plus_minus_accumulates_over_goals(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);

	scoreboard_increment_home_score();
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
}

static void test_goal_with_nobody_on_ice(void)
{
	setup_rosters();
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
	assert(scoreboard_get_home_score() == 1);
}

static void test_goal_with_empty_rosters(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_score() == 1);
	assert(scoreboard_get_away_score() == 1);
}

static void test_power_play_goal_skipped_by_default(void)
{
	setup_rosters();
	assert(scoreboard_get_plus_minus_skip_power_play());
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	/* Away is shorthanded, so a home goal is a power-play goal. */
	scoreboard_away_penalty_add(22, 120);

	scoreboard_increment_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
	assert(scoreboard_get_home_score() == 1);
}

static void test_away_power_play_goal_skipped(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_home_penalty_add(12, 120);

	scoreboard_increment_away_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
}

static void test_shorthanded_goal_counts(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	/* Home is shorthanded and scores. */
	scoreboard_home_penalty_add(12, 120);

	scoreboard_increment_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
}

static void test_matching_penalties_count_as_even_strength(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_home_penalty_add(12, 120);
	scoreboard_away_penalty_add(22, 120);

	scoreboard_increment_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
}

static void test_power_play_goal_counts_when_skip_disabled(void)
{
	setup_rosters();
	scoreboard_set_plus_minus_skip_power_play(false);
	assert(!scoreboard_get_plus_minus_skip_power_play());
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_away_penalty_add(22, 120);

	scoreboard_increment_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
}

static void test_goal_adds_action_log_entry(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_player_set_on_ice(false, 21, true);

	scoreboard_increment_home_score();

	char logs[2048];
	scoreboard_copy_action_logs(logs, sizeof(logs));
	assert(strstr(logs, "Plus/minus: +1 for 1, -1 for 2") != NULL);
}

/* ---- taking goals back ---- */

static void test_decrement_reverses_goal(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);

	scoreboard_decrement_home_score();

	assert(scoreboard_get_home_score() == 0);
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
}

static void test_decrement_away_reverses_goal(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_away_score();

	scoreboard_decrement_away_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
}

static void test_decrement_reverses_original_players_not_current(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_increment_home_score();
	/* Line change before the goal is taken back. */
	scoreboard_player_set_on_ice(true, 10, false);
	scoreboard_player_set_on_ice(true, 11, true);

	scoreboard_decrement_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(true, 11) == 0);
}

static void test_decrement_reverses_most_recent_goal_first(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_increment_home_score();
	scoreboard_player_set_on_ice(true, 10, false);
	scoreboard_player_set_on_ice(true, 11, true);
	scoreboard_increment_home_score();

	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(true, 11) == 0);

	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

static void test_decrement_skips_other_teams_goals(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	/* Net: 10 = +1 - 1 = 0, 20 = -1 + 1 = 0. */

	scoreboard_decrement_home_score();

	/* Only the home goal is reversed; the away goal still stands. */
	assert(scoreboard_player_get_plus_minus(true, 10) == -1);
	assert(scoreboard_player_get_plus_minus(false, 20) == 1);
}

static void test_decrement_at_zero_changes_nothing(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_decrement_home_score();
	assert(scoreboard_get_home_score() == 0);
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

static void test_decrement_of_skipped_power_play_goal_changes_nothing(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_home_score();

	scoreboard_decrement_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

static void test_decrement_with_removed_player(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();
	scoreboard_roster_remove(true, 10);
	scoreboard_roster_remove(false, 20);

	scoreboard_decrement_home_score();

	assert(scoreboard_get_home_score() == 0);
	assert(scoreboard_roster_count(true) == 2);
}

static void test_decrement_without_recorded_goal(void)
{
	setup_rosters();
	scoreboard_set_home_score(3);
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_decrement_home_score();
	assert(scoreboard_get_home_score() == 2);
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

static void test_set_score_forgets_that_teams_goals(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();

	/* After both goals each player nets 0 (+1 and -1). */
	scoreboard_set_home_score(5);
	scoreboard_decrement_home_score();
	/* Home goal history was dropped, so nothing is reversed. */
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);

	/* The away goal can still be reversed. */
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(false, 20) == -1);
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
}

static void test_set_away_score_forgets_away_goals(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_away_score();
	scoreboard_increment_home_score();

	scoreboard_set_away_score(4);
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);

	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == -1);
}

static void test_set_score_to_same_value_keeps_goal_history(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();

	/* Re-applying the current scores (for example when files are re-read)
	   must not break the link to recorded goals. */
	scoreboard_set_home_score(1);
	scoreboard_set_away_score(1);

	scoreboard_decrement_home_score();
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

static void test_goal_history_is_bounded(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	for (int i = 0; i < 17; i++)
		scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 17);

	for (int i = 0; i < 17; i++)
		scoreboard_decrement_home_score();

	/* Only the 16 most recent goals can be reversed. */
	assert(scoreboard_get_home_score() == 0);
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
}

/* ---- manual changes ---- */

static void test_adjust_and_reset_plus_minus(void)
{
	setup_rosters();
	assert(scoreboard_player_adjust_plus_minus(true, 10, 3));
	assert(scoreboard_player_adjust_plus_minus(true, 11, -2));
	assert(scoreboard_player_get_plus_minus(true, 10) == 3);
	assert(scoreboard_player_get_plus_minus(true, 11) == -2);
	assert(!scoreboard_player_adjust_plus_minus(true, 99, 1));
	assert(scoreboard_player_get_plus_minus(true, 99) == 0);

	scoreboard_player_adjust_plus_minus(false, 20, 1);
	scoreboard_roster_reset_plus_minus(true);
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(true, 11) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 1);
}

static void test_reset_plus_minus_drops_goal_history(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_increment_home_score();
	scoreboard_roster_reset_plus_minus(true);

	scoreboard_decrement_home_score();

	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
}

/* ---- formatting ---- */

static void test_format_plus_minus_values(void)
{
	char buf[16];
	scoreboard_format_plus_minus(3, buf, sizeof(buf));
	assert(strcmp(buf, "+3") == 0);
	scoreboard_format_plus_minus(-2, buf, sizeof(buf));
	assert(strcmp(buf, "-2") == 0);
	scoreboard_format_plus_minus(0, buf, sizeof(buf));
	assert(strcmp(buf, "0") == 0);
}

static void test_format_lines_all_and_on_ice(void)
{
	setup_rosters();
	scoreboard_player_adjust_plus_minus(true, 10, 2);
	scoreboard_player_adjust_plus_minus(true, 12, -1);
	scoreboard_player_set_on_ice(true, 12, true);
	char buf[256];

	scoreboard_format_plus_minus_lines(true, true, buf, sizeof(buf));
	assert(strcmp(buf, "#10  +2\n#11  0\n#12  -1") == 0);

	scoreboard_format_plus_minus_lines(true, false, buf, sizeof(buf));
	assert(strcmp(buf, "#12  -1") == 0);

	scoreboard_format_plus_minus_lines(false, false, buf, sizeof(buf));
	assert(buf[0] == '\0');
}

static void test_format_lines_empty_roster(void)
{
	scoreboard_reset_state_for_tests();
	char buf[32] = "junk";
	scoreboard_format_plus_minus_lines(true, true, buf, sizeof(buf));
	assert(buf[0] == '\0');
}

static void test_format_lines_truncates_cleanly(void)
{
	setup_rosters();
	char buf[10];
	/* "#10  0" fits (6 chars); adding "\n#11  0" would not. */
	scoreboard_format_plus_minus_lines(true, true, buf, sizeof(buf));
	assert(strcmp(buf, "#10  0") == 0);

	char tiny[4];
	scoreboard_format_plus_minus_lines(true, true, tiny, sizeof(tiny));
	assert(tiny[0] == '\0');

	char zero[1] = {'x'};
	scoreboard_format_plus_minus_lines(true, true, zero, 0);
	assert(zero[0] == 'x');
}

/* ---- roster text form ---- */

static void test_roster_to_string(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 11, true);
	scoreboard_player_adjust_plus_minus(true, 12, -3);
	char buf[128];

	scoreboard_roster_to_string(true, buf, sizeof(buf));
	assert(strcmp(buf, "10:0:0,11:1:0,12:0:-3") == 0);

	scoreboard_roster_clear(false);
	scoreboard_roster_to_string(false, buf, sizeof(buf));
	assert(buf[0] == '\0');
}

static void test_roster_to_string_truncates_cleanly(void)
{
	setup_rosters();
	char buf[10];
	/* "10:0:0" fits (6 chars); adding ",11:0:0" would not. */
	scoreboard_roster_to_string(true, buf, sizeof(buf));
	assert(strcmp(buf, "10:0:0") == 0);

	char zero[1] = {'x'};
	scoreboard_roster_to_string(true, zero, 0);
	assert(zero[0] == 'x');
}

static void test_roster_from_string(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(true, 77);
	scoreboard_roster_from_string(true, "10:1:2,11:0:-1,12");

	assert(scoreboard_roster_count(true) == 3);
	assert(scoreboard_roster_get(true, 0)->number == 10);
	assert(scoreboard_roster_get(true, 0)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 10) == 2);
	assert(!scoreboard_roster_get(true, 1)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 11) == -1);
	assert(scoreboard_roster_get(true, 2)->number == 12);
	assert(!scoreboard_roster_get(true, 2)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 12) == 0);
	/* The old roster was replaced. */
	assert(!scoreboard_roster_find(true, 77));
}

static void test_roster_from_string_round_trip(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(false, 21, true);
	scoreboard_player_adjust_plus_minus(false, 22, 5);
	char buf[128];
	scoreboard_roster_to_string(false, buf, sizeof(buf));

	scoreboard_roster_from_string(false, buf);

	assert(scoreboard_roster_count(false) == 3);
	assert(scoreboard_roster_get(false, 1)->on_ice);
	assert(scoreboard_player_get_plus_minus(false, 22) == 5);
	/* The home roster is untouched. */
	assert(scoreboard_roster_count(true) == 3);
}

static void test_roster_from_string_skips_bad_entries(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_from_string(
		true, "abc,,7:1:3:junk,-5:0:0,1000:0:0, 8:1,9:x,4294967299:0:0");

	/* 7 (with trailing junk), 8 and 9 survive. The rest are skipped. */
	assert(scoreboard_roster_count(true) == 3);
	assert(scoreboard_roster_get(true, 0)->number == 7);
	assert(scoreboard_roster_get(true, 0)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 7) == 3);
	assert(scoreboard_roster_get(true, 1)->number == 8);
	assert(scoreboard_roster_get(true, 1)->on_ice);
	assert(scoreboard_roster_get(true, 2)->number == 9);
	assert(!scoreboard_roster_get(true, 2)->on_ice);
}

static void test_roster_from_string_duplicates_and_full(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_from_string(true, "5:0:1,5:1:2");
	assert(scoreboard_roster_count(true) == 1);
	assert(scoreboard_roster_get(true, 0)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 5) == 2);

	char text[512] = "";
	for (int i = 0; i < SCOREBOARD_MAX_ROSTER + 5; i++) {
		char item[16];
		snprintf(item, sizeof(item), "%d,", i + 1);
		strcat(text, item);
	}
	scoreboard_roster_from_string(true, text);
	assert(scoreboard_roster_count(true) == SCOREBOARD_MAX_ROSTER);
}

static void test_roster_from_string_null_and_empty(void)
{
	setup_rosters();
	scoreboard_roster_from_string(true, NULL);
	assert(scoreboard_roster_count(true) == 0);
	assert(scoreboard_roster_count(false) == 3);

	scoreboard_roster_from_string(false, "");
	assert(scoreboard_roster_count(false) == 0);
}

/* ---- file output ---- */

static void test_files_written(void)
{
	setup_tmp_dir();
	setup_rosters();
	scoreboard_set_output_directory(g_tmp_dir);
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();
	assert(scoreboard_write_all_files());

	expect_file("home_plus_minus.txt", "#10  +1\n#11  0\n#12  0");
	expect_file("away_plus_minus.txt", "#20  -1\n#21  0\n#22  0");
	expect_file("home_on_ice.txt", "#10  +1");
	expect_file("away_on_ice.txt", "#20  -1");
	cleanup_tmp_dir();
}

static void test_files_empty_without_roster(void)
{
	setup_tmp_dir();
	scoreboard_reset_state_for_tests();
	scoreboard_set_output_directory(g_tmp_dir);
	scoreboard_mark_dirty();
	assert(scoreboard_write_all_files());
	expect_file("home_plus_minus.txt", "");
	expect_file("away_on_ice.txt", "");
	cleanup_tmp_dir();
}

/* ---- persistence ---- */

static void test_save_and_load_round_trip(void)
{
	setup_tmp_dir();
	setup_rosters();
	scoreboard_set_plus_minus_skip_power_play(false);
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 21, true);
	scoreboard_increment_home_score();
	scoreboard_player_adjust_plus_minus(true, 12, 4);

	char path[512];
	snprintf(path, sizeof(path), "%s/state.json", g_tmp_dir);
	assert(scoreboard_save_state(path));

	scoreboard_reset_state_for_tests();
	assert(scoreboard_get_plus_minus_skip_power_play());
	assert(scoreboard_load_state(path));

	assert(!scoreboard_get_plus_minus_skip_power_play());
	assert(scoreboard_roster_count(true) == 3);
	assert(scoreboard_roster_count(false) == 3);
	assert(scoreboard_roster_get(true, 0)->number == 10);
	assert(scoreboard_roster_get(true, 0)->on_ice);
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	assert(scoreboard_player_get_plus_minus(true, 12) == 4);
	assert(scoreboard_roster_get(false, 1)->number == 21);
	assert(scoreboard_roster_get(false, 1)->on_ice);
	assert(scoreboard_player_get_plus_minus(false, 21) == -1);
	assert(!scoreboard_roster_get(false, 0)->on_ice);
	cleanup_tmp_dir();
}

static void test_load_older_state_keeps_roster(void)
{
	setup_tmp_dir();
	setup_rosters();
	scoreboard_player_adjust_plus_minus(true, 10, 2);

	char path[512];
	snprintf(path, sizeof(path), "%s/old.json", g_tmp_dir);
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_score\": 4\n}\n");
	fclose(f);

	assert(scoreboard_load_state(path));
	assert(scoreboard_get_home_score() == 4);
	assert(scoreboard_roster_count(true) == 3);
	assert(scoreboard_player_get_plus_minus(true, 10) == 2);
	assert(scoreboard_get_plus_minus_skip_power_play());
	cleanup_tmp_dir();
}

static void test_load_clamps_roster_count(void)
{
	setup_tmp_dir();
	scoreboard_reset_state_for_tests();

	char path[512];
	snprintf(path, sizeof(path), "%s/big.json", g_tmp_dir);
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_roster_count\": 500,\n"
		   "  \"home_player0_number\": 7\n}\n");
	fclose(f);

	assert(scoreboard_load_state(path));
	assert(scoreboard_roster_count(true) == SCOREBOARD_MAX_ROSTER);
	assert(scoreboard_roster_get(true, 0)->number == 7);
	assert(scoreboard_roster_count(false) == 0);
	cleanup_tmp_dir();
}

static void test_load_drops_goal_history(void)
{
	setup_tmp_dir();
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_increment_home_score();

	char path[512];
	snprintf(path, sizeof(path), "%s/hist.json", g_tmp_dir);
	assert(scoreboard_save_state(path));
	assert(scoreboard_load_state(path));

	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(true, 10) == 1);
	cleanup_tmp_dir();
}

/* ---- new game ---- */

static void test_new_game_resets_plus_minus_but_keeps_rosters(void)
{
	setup_rosters();
	scoreboard_player_set_on_ice(true, 10, true);
	scoreboard_player_set_on_ice(false, 20, true);
	scoreboard_increment_home_score();

	scoreboard_new_game();

	assert(scoreboard_roster_count(true) == 3);
	assert(scoreboard_roster_count(false) == 3);
	assert(scoreboard_roster_on_ice_count(true) == 0);
	assert(scoreboard_roster_on_ice_count(false) == 0);
	assert(scoreboard_player_get_plus_minus(true, 10) == 0);
	assert(scoreboard_player_get_plus_minus(false, 20) == 0);
}

int main(void)
{
	test_roster_add_and_get();
	test_roster_add_duplicate_returns_existing();
	test_roster_add_rejects_bad_numbers();
	test_roster_full();
	test_roster_get_out_of_range();
	test_roster_remove_keeps_order();
	test_roster_clear();
	test_roster_marks_dirty();

	test_on_ice_set_toggle_clear();
	test_on_ice_unknown_player();

	test_home_goal_credits_on_ice_players();
	test_away_goal_credits_on_ice_players();
	test_plus_minus_accumulates_over_goals();
	test_goal_with_nobody_on_ice();
	test_goal_with_empty_rosters();
	test_power_play_goal_skipped_by_default();
	test_away_power_play_goal_skipped();
	test_shorthanded_goal_counts();
	test_matching_penalties_count_as_even_strength();
	test_power_play_goal_counts_when_skip_disabled();
	test_goal_adds_action_log_entry();

	test_decrement_reverses_goal();
	test_decrement_away_reverses_goal();
	test_decrement_reverses_original_players_not_current();
	test_decrement_reverses_most_recent_goal_first();
	test_decrement_skips_other_teams_goals();
	test_decrement_at_zero_changes_nothing();
	test_decrement_of_skipped_power_play_goal_changes_nothing();
	test_decrement_with_removed_player();
	test_decrement_without_recorded_goal();
	test_set_score_forgets_that_teams_goals();
	test_set_away_score_forgets_away_goals();
	test_set_score_to_same_value_keeps_goal_history();
	test_goal_history_is_bounded();

	test_adjust_and_reset_plus_minus();
	test_reset_plus_minus_drops_goal_history();

	test_format_plus_minus_values();
	test_format_lines_all_and_on_ice();
	test_format_lines_empty_roster();
	test_format_lines_truncates_cleanly();

	test_roster_to_string();
	test_roster_to_string_truncates_cleanly();
	test_roster_from_string();
	test_roster_from_string_round_trip();
	test_roster_from_string_skips_bad_entries();
	test_roster_from_string_duplicates_and_full();
	test_roster_from_string_null_and_empty();

	test_files_written();
	test_files_empty_without_roster();

	test_save_and_load_round_trip();
	test_load_older_state_keeps_roster();
	test_load_clamps_roster_count();
	test_load_drops_goal_history();

	test_new_game_resets_plus_minus_but_keeps_rosters();

	printf("All plus/minus tests passed\n");
	return 0;
}
