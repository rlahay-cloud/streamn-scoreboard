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

/* Home roster 10, 11, 12. */
static void setup_roster(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(10);
	scoreboard_roster_add(11);
	scoreboard_roster_add(12);
}

static const struct scoreboard_player *player(int number)
{
	for (int i = 0; i < scoreboard_roster_count(); i++) {
		const struct scoreboard_player *p = scoreboard_roster_get(i);
		if (p->number == number)
			return p;
	}
	return NULL;
}

/* ---- roster ---- */

static void test_roster_add_get_remove_clear(void)
{
	scoreboard_reset_state_for_tests();
	assert(scoreboard_roster_count() == 0);
	assert(scoreboard_roster_add(17) == 0);
	assert(scoreboard_roster_add(4) == 1);
	assert(scoreboard_roster_add(17) == 0);
	assert(scoreboard_roster_count() == 2);
	assert(scoreboard_roster_find(4));
	assert(!scoreboard_roster_find(5));
	assert(scoreboard_roster_get(0)->number == 17);
	assert(scoreboard_roster_get(-1) == NULL);
	assert(scoreboard_roster_get(2) == NULL);

	assert(scoreboard_roster_add(-1) == -1);
	assert(scoreboard_roster_add(SCOREBOARD_MAX_PLAYER_NUMBER + 1) == -1);
	assert(scoreboard_roster_add(0) == 2);

	assert(scoreboard_roster_remove(17));
	assert(!scoreboard_roster_remove(17));
	assert(scoreboard_roster_get(0)->number == 4);
	assert(scoreboard_roster_get(1)->number == 0);

	scoreboard_roster_clear();
	assert(scoreboard_roster_count() == 0);
}

static void test_roster_full(void)
{
	scoreboard_reset_state_for_tests();
	for (int i = 0; i < SCOREBOARD_MAX_ROSTER; i++)
		assert(scoreboard_roster_add(i + 1) == i);
	assert(scoreboard_roster_add(500) == -1);
	assert(scoreboard_roster_count() == SCOREBOARD_MAX_ROSTER);
}

static void test_roster_marks_dirty(void)
{
	setup_tmp_dir();
	setup_roster();
	scoreboard_set_output_directory(g_tmp_dir);
	assert(scoreboard_write_all_files());
	assert(!scoreboard_is_dirty());
	scoreboard_roster_add(13);
	assert(scoreboard_is_dirty());
	cleanup_tmp_dir();
}

static void test_on_ice(void)
{
	setup_roster();
	assert(scoreboard_roster_on_ice_count() == 0);
	assert(scoreboard_player_set_on_ice(10, true));
	assert(scoreboard_player_toggle_on_ice(11));
	assert(scoreboard_roster_on_ice_count() == 2);
	assert(scoreboard_player_toggle_on_ice(11));
	assert(scoreboard_roster_on_ice_count() == 1);
	assert(!scoreboard_player_set_on_ice(99, true));
	assert(!scoreboard_player_toggle_on_ice(99));
	scoreboard_roster_clear_on_ice();
	assert(scoreboard_roster_on_ice_count() == 0);
}

/* ---- plus/minus from goals ---- */

static void test_home_goal_gives_plus_one_to_on_ice(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_player_set_on_ice(11, true);
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 1);
	assert(scoreboard_player_get_plus_minus(11) == 1);
	assert(scoreboard_player_get_plus_minus(12) == 0);
	assert(player(10)->season_plus_minus == 1);
}

static void test_away_goal_gives_minus_one_to_on_ice(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(12, true);
	scoreboard_increment_away_score();
	assert(scoreboard_player_get_plus_minus(12) == -1);
	assert(scoreboard_player_get_plus_minus(10) == 0);
	assert(player(12)->season_plus_minus == -1);
}

static void test_goal_with_nobody_on_ice_or_no_roster(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	scoreboard_reset_state_for_tests();
	scoreboard_increment_home_score();
	assert(scoreboard_get_home_score() == 1);
}

static void test_no_plus_minus_while_a_penalty_is_active(void)
{
	setup_roster();
	assert(scoreboard_get_plus_minus_skip_power_play());
	scoreboard_player_set_on_ice(10, true);

	/* away penalty: a home goal gives nothing */
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	scoreboard_away_penalty_clear(0);

	/* home penalty: an away goal gives nothing */
	scoreboard_home_penalty_add(12, 120);
	scoreboard_increment_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);

	/* a shorthanded goal gives nothing either */
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);

	/* matching penalties (4 on 4) give nothing */
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);

	/* once every penalty is over, goals count again */
	scoreboard_home_penalty_clear(0);
	scoreboard_away_penalty_clear(0);
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 1);
}

static void test_power_play_goals_count_when_skip_disabled(void)
{
	setup_roster();
	scoreboard_set_plus_minus_skip_power_play(false);
	assert(!scoreboard_get_plus_minus_skip_power_play());
	scoreboard_player_set_on_ice(10, true);
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 1);
}

static void test_goal_adds_action_log_entry(void)
{
	char logs[2048];
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_player_set_on_ice(11, true);
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	scoreboard_copy_action_logs(logs, sizeof(logs));
	assert(strstr(logs, "Plus/minus: +1 for 2 on-ice players") != NULL);
	assert(strstr(logs, "Plus/minus: -1 for 2 on-ice players") != NULL);
}

/* ---- taking goals back ---- */

static void test_decrement_reverses_goal_for_original_players(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_player_set_on_ice(10, false);
	scoreboard_player_set_on_ice(11, true);
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	assert(scoreboard_player_get_plus_minus(11) == 0);
	assert(player(10)->season_plus_minus == 0);
}

static void test_decrement_reverses_away_goal(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_away_score();
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
}

static void test_decrement_reverses_most_recent_goal_first(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_player_set_on_ice(10, false);
	scoreboard_player_set_on_ice(11, true);
	scoreboard_increment_home_score();
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 1);
	assert(scoreboard_player_get_plus_minus(11) == 0);
}

static void test_decrement_skips_other_teams_goals(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == -1);
}

static void test_decrement_edge_cases(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	/* at zero: nothing changes */
	scoreboard_decrement_home_score();
	assert(scoreboard_get_home_score() == 0);
	/* skipped power-play goal: nothing to reverse */
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_home_score();
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	/* player removed from the roster before the goal is taken back */
	scoreboard_away_penalty_clear(0);
	scoreboard_increment_home_score();
	scoreboard_roster_remove(10);
	scoreboard_decrement_home_score();
	assert(!scoreboard_roster_find(10));
	/* score typed in, so no recorded goal exists */
	scoreboard_set_home_score(5);
	scoreboard_decrement_home_score();
	assert(scoreboard_get_home_score() == 4);
}

static void test_set_score_forgets_goals_only_on_change(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_set_home_score(1); /* same value: history kept */
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);

	scoreboard_increment_home_score();
	scoreboard_set_home_score(7); /* changed: history dropped */
	scoreboard_decrement_home_score();
	assert(scoreboard_player_get_plus_minus(10) == 1);

	scoreboard_increment_away_score();
	scoreboard_set_away_score(3);
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	/* typing in one team's score keeps the other team's history */
	scoreboard_increment_away_score();
	scoreboard_set_home_score(9);
	scoreboard_decrement_away_score();
	assert(scoreboard_player_get_plus_minus(10) == 0);
	scoreboard_set_home_score(-4);
	assert(scoreboard_get_home_score() == 0);
	scoreboard_set_away_score(-4);
	assert(scoreboard_get_away_score() == 0);
}

static void test_goal_history_is_bounded(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	for (int i = 0; i < 17; i++)
		scoreboard_increment_home_score();
	for (int i = 0; i < 17; i++)
		scoreboard_decrement_home_score();
	/* Only the 16 most recent goals can be reversed. */
	assert(scoreboard_player_get_plus_minus(10) == 1);
}

/* ---- manual edits and season totals ---- */

static void test_manual_game_edits_move_season_too(void)
{
	setup_roster();
	assert(scoreboard_player_adjust_plus_minus(10, 3));
	assert(scoreboard_player_adjust_plus_minus(10, -1));
	assert(player(10)->plus_minus == 2 && player(10)->season_plus_minus == 2);
	assert(scoreboard_player_set_plus_minus(10, 7));
	assert(player(10)->plus_minus == 7 && player(10)->season_plus_minus == 7);
	assert(scoreboard_player_set_plus_minus(10, -3));
	assert(player(10)->season_plus_minus == -3);

	assert(scoreboard_player_set_goals(10, 2));
	assert(scoreboard_player_set_assists(10, 3));
	assert(player(10)->season_goals == 2 && player(10)->season_assists == 3);
	assert(scoreboard_player_set_goals(10, -4));
	assert(scoreboard_player_set_assists(10, -4));
	assert(player(10)->goals == 0 && player(10)->assists == 0);
	assert(player(10)->season_goals == 0 && player(10)->season_assists == 0);

	assert(!scoreboard_player_adjust_plus_minus(99, 1));
	assert(!scoreboard_player_set_plus_minus(99, 1));
	assert(!scoreboard_player_set_goals(99, 1));
	assert(!scoreboard_player_set_assists(99, 1));
	assert(!scoreboard_player_set_season(99, 1, 1, 1));
	assert(scoreboard_player_get_plus_minus(99) == 0);
	assert(scoreboard_player_get_goals(99) == 0);
	assert(scoreboard_player_get_assists(99) == 0);
}

static void test_set_season_directly(void)
{
	setup_roster();
	scoreboard_player_set_goals(10, 1);
	assert(scoreboard_player_set_season(10, 14, 9, 12));
	assert(player(10)->season_plus_minus == 14);
	assert(player(10)->season_goals == 9);
	assert(player(10)->season_assists == 12);
	assert(player(10)->goals == 1);
	assert(scoreboard_player_set_season(10, -2, -5, -5));
	assert(player(10)->season_plus_minus == -2);
	assert(player(10)->season_goals == 0 && player(10)->season_assists == 0);
	/* a game goal still adds to the season */
	scoreboard_player_set_season(10, 0, 9, 0);
	scoreboard_player_set_goals(10, 2);
	assert(player(10)->season_goals == 10);
}

static void test_season_goal_edit_never_below_zero(void)
{
	setup_roster();
	scoreboard_player_set_goals(10, 3);
	scoreboard_player_set_season(10, 0, 1, 1);
	scoreboard_player_set_goals(10, 0);
	assert(player(10)->season_goals == 0);
	scoreboard_player_set_assists(10, 2);
	scoreboard_player_set_season(10, 0, 0, 0);
	scoreboard_player_set_assists(10, 0);
	assert(player(10)->season_assists == 0);
}

static void test_reset_game_and_season(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_player_set_goals(10, 2);
	scoreboard_player_set_assists(10, 1);
	scoreboard_roster_reset_game_stats();
	assert(player(10)->plus_minus == 0 && player(10)->goals == 0 &&
	       player(10)->assists == 0);
	assert(player(10)->season_plus_minus == 1);
	assert(player(10)->season_goals == 2);
	assert(player(10)->season_assists == 1);
	/* the goal history was dropped */
	scoreboard_decrement_home_score();
	assert(player(10)->season_plus_minus == 1);

	scoreboard_roster_reset_season_stats();
	assert(player(10)->season_plus_minus == 0);
	assert(player(10)->season_goals == 0);
	assert(player(10)->season_assists == 0);
}

static void test_new_game_keeps_season_and_roster(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_player_set_goals(10, 1);
	scoreboard_new_game();
	assert(scoreboard_roster_count() == 3);
	assert(!player(10)->on_ice);
	assert(player(10)->plus_minus == 0 && player(10)->goals == 0);
	assert(player(10)->season_plus_minus == 1);
	assert(player(10)->season_goals == 1);
}

/* ---- crediting goals ---- */

static void test_credit_goal(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, 11, 12));
	assert(player(10)->goals == 1 && player(10)->season_goals == 1);
	assert(player(11)->assists == 1 && player(12)->assists == 1);
	assert(player(12)->season_assists == 1);
	assert(player(10)->assists == 0);
}

static void test_credit_goal_optional_parts(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, -1, -1));
	assert(player(10)->goals == 1);
	assert(scoreboard_credit_goal(-1, 11, -1));
	assert(player(10)->goals == 0);
	assert(player(11)->assists == 1);
}

static void test_credit_goal_rejects_bad_input(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(!scoreboard_credit_goal(99, -1, -1));
	assert(!scoreboard_credit_goal(10, 99, -1));
	assert(!scoreboard_credit_goal(10, 10, -1));
	assert(!scoreboard_credit_goal(10, 11, 10));
	assert(!scoreboard_credit_goal(10, 11, 11));
	assert(player(10)->goals == 0 && player(11)->assists == 0);
}

static void test_credit_goal_correction_replaces_credit(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, 11, -1));
	assert(scoreboard_credit_goal(12, -1, 10));
	assert(player(10)->goals == 0 && player(11)->assists == 0);
	assert(player(12)->goals == 1 && player(10)->assists == 1);
	assert(player(10)->season_goals == 0);
}

static void test_credit_goal_goes_to_latest_home_goal(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, -1, -1));
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(11, -1, -1));
	scoreboard_increment_away_score();
	scoreboard_decrement_home_score();
	assert(player(11)->goals == 0);
	assert(player(10)->goals == 1);
}

static void test_removing_goal_removes_credit(void)
{
	int s, a, b;
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, 11, 12));
	scoreboard_decrement_home_score();
	assert(player(10)->goals == 0 && player(11)->assists == 0 &&
	       player(12)->assists == 0);
	assert(player(10)->season_goals == 0);
	assert(!scoreboard_get_last_goal(&s, &a, &b));
}

static void test_removing_goal_never_goes_below_zero(void)
{
	setup_roster();
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, 11, -1));
	scoreboard_player_set_goals(10, 0);
	scoreboard_player_set_assists(11, 0);
	scoreboard_decrement_home_score();
	assert(player(10)->goals == 0 && player(11)->assists == 0);
	assert(player(10)->season_goals == 0);
}

static void test_credit_goal_without_recorded_goal(void)
{
	int s, a, b;
	setup_roster();
	assert(scoreboard_credit_goal(10, -1, -1));
	assert(player(10)->goals == 1);
	assert(!scoreboard_get_last_goal(&s, &a, &b));
}

static void test_last_goal_and_format(void)
{
	char buf[128];
	int s = 0, a1 = 0, a2 = 0;
	setup_roster();
	scoreboard_set_home_name("Eagles");
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "") == 0);
	scoreboard_format_last_goal(buf, 0);

	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, -1, -1));
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "Eagles goal: #10 (unassisted)") == 0);

	assert(scoreboard_credit_goal(10, -1, 12));
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "Eagles goal: #10 (assist: #12)") == 0);
	assert(scoreboard_credit_goal(10, 11, -1));
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "Eagles goal: #10 (assist: #11)") == 0);

	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(11, 10, 12));
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "Eagles goal: #11 (assists: #10, #12)") == 0);
	assert(scoreboard_get_last_goal(&s, &a1, &a2));
	assert(s == 11 && a1 == 10 && a2 == 12);

	/* newer goals without credit are skipped */
	scoreboard_increment_home_score();
	scoreboard_increment_away_score();
	scoreboard_format_last_goal(buf, sizeof(buf));
	assert(strcmp(buf, "Eagles goal: #11 (assists: #10, #12)") == 0);
}

/* ---- text output ---- */

static void test_format_plus_minus_values(void)
{
	char buf[16];
	scoreboard_format_plus_minus(0, buf, sizeof(buf));
	assert(strcmp(buf, "0") == 0);
	scoreboard_format_plus_minus(3, buf, sizeof(buf));
	assert(strcmp(buf, "+3") == 0);
	scoreboard_format_plus_minus(-2, buf, sizeof(buf));
	assert(strcmp(buf, "-2") == 0);
}

static void test_plus_minus_lines(void)
{
	char buf[256];
	setup_roster();
	scoreboard_player_adjust_plus_minus(10, 2);
	scoreboard_player_set_on_ice(11, true);
	scoreboard_player_adjust_plus_minus(11, -1);
	scoreboard_player_set_season(10, 14, 0, 0);

	scoreboard_format_plus_minus_lines(true, false, buf, sizeof(buf));
	assert(strcmp(buf, "#10    +2\n#11    -1\n#12     0") == 0);
	scoreboard_format_plus_minus_lines(false, false, buf, sizeof(buf));
	assert(strcmp(buf, "#11    -1") == 0);
	scoreboard_format_plus_minus_lines(true, true, buf, sizeof(buf));
	assert(strcmp(buf, "#10   +14\n#11    -1\n#12     0") == 0);

	/* only whole lines fit */
	scoreboard_format_plus_minus_lines(true, false, buf, 15);
	assert(strcmp(buf, "#10    +2") == 0);
	buf[0] = 'x';
	scoreboard_format_plus_minus_lines(true, false, buf, 0);
	assert(buf[0] == 'x');

	scoreboard_roster_clear();
	scoreboard_format_plus_minus_lines(true, false, buf, sizeof(buf));
	assert(buf[0] == '\0');
}

static void test_scoring_lines(void)
{
	char buf[256];
	setup_roster();
	scoreboard_format_scoring_lines(false, buf, 0);
	scoreboard_format_scoring_lines(false, buf, sizeof(buf));
	assert(strcmp(buf, "") == 0);
	scoreboard_player_set_goals(10, 2);
	scoreboard_player_set_assists(12, 1);
	scoreboard_player_set_season(12, 0, 5, 7);
	scoreboard_format_scoring_lines(false, buf, sizeof(buf));
	assert(strcmp(buf, "#10   2G  0A  2P\n#12   0G  1A  1P") == 0);
	scoreboard_format_scoring_lines(true, buf, sizeof(buf));
	assert(strcmp(buf, "#10   2G  0A  2P\n#11   0G  0A  0P\n#12   5G  7A 12P") == 0);
	scoreboard_format_scoring_lines(false, buf, 25);
	assert(strcmp(buf, "#10   2G  0A  2P") == 0);
}

static void test_files_written(void)
{
	setup_tmp_dir();
	setup_roster();
	scoreboard_set_home_name("Eagles");
	scoreboard_set_output_directory(g_tmp_dir);
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	assert(scoreboard_credit_goal(10, 11, -1));
	scoreboard_player_set_season(10, 5, 3, 4);
	assert(scoreboard_write_all_files());

	expect_file("home_plus_minus.txt", "#10    +1\n#11     0\n#12     0");
	expect_file("home_season_plus_minus.txt",
		    "#10    +5\n#11     0\n#12     0");
	expect_file("home_on_ice.txt", "#10    +1");
	expect_file("home_scoring.txt", "#10   1G  0A  1P\n#11   0G  1A  1P");
	expect_file("home_season_scoring.txt",
		    "#10   3G  4A  7P\n#11   0G  1A  1P\n#12   0G  0A  0P");
	expect_file("last_goal.txt", "Eagles goal: #10 (assist: #11)");
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
	expect_file("home_on_ice.txt", "");
	expect_file("home_season_scoring.txt", "");
	expect_file("last_goal.txt", "");
	cleanup_tmp_dir();
}

/* ---- roster text form ---- */

static void test_roster_string_round_trip(void)
{
	char buf[256];
	setup_roster();
	scoreboard_player_set_on_ice(11, true);
	scoreboard_player_adjust_plus_minus(12, -3);
	scoreboard_player_set_goals(10, 2);
	scoreboard_player_set_assists(10, 3);
	scoreboard_player_set_season(10, 9, 8, 7);

	scoreboard_roster_to_string(buf, sizeof(buf));
	assert(strcmp(buf, "10:0:0:2:3:9:8:7,11:1:0:0:0:0:0:0,"
			   "12:0:-3:0:0:-3:0:0") == 0);

	scoreboard_roster_clear();
	scoreboard_roster_to_string(buf, sizeof(buf));
	assert(buf[0] == '\0');

	setup_roster();
	scoreboard_player_set_on_ice(11, true);
	scoreboard_player_adjust_plus_minus(12, -3);
	scoreboard_player_set_goals(10, 2);
	scoreboard_player_set_season(10, 9, 8, 7);
	scoreboard_roster_to_string(buf, sizeof(buf));
	scoreboard_roster_clear();
	scoreboard_roster_from_string(buf);
	assert(scoreboard_roster_count() == 3);
	assert(player(11)->on_ice);
	assert(player(12)->plus_minus == -3 && player(12)->season_plus_minus == -3);
	assert(player(10)->goals == 2);
	assert(player(10)->season_plus_minus == 9);
	assert(player(10)->season_goals == 8);
	assert(player(10)->season_assists == 7);
}

static void test_roster_string_truncates_cleanly(void)
{
	char buf[20];
	char zero[1] = {'x'};
	setup_roster();
	/* "10:0:0:0:0:0:0:0" is 16 chars; a second entry would not fit. */
	scoreboard_roster_to_string(buf, sizeof(buf));
	assert(strcmp(buf, "10:0:0:0:0:0:0:0") == 0);
	scoreboard_roster_to_string(zero, 0);
	assert(zero[0] == 'x');
}

static void test_roster_string_older_forms_load(void)
{
	setup_roster();
	scoreboard_roster_add(77);
	scoreboard_roster_from_string("10:1:2,11:0:-1:3:4,12");
	assert(scoreboard_roster_count() == 3);
	assert(!scoreboard_roster_find(77));
	assert(player(10)->on_ice && player(10)->plus_minus == 2);
	/* no season numbers saved: season starts from the game values */
	assert(player(10)->season_plus_minus == 2);
	assert(player(11)->plus_minus == -1 && player(11)->goals == 3);
	assert(player(11)->assists == 4);
	assert(player(11)->season_goals == 3 && player(11)->season_assists == 4);
	assert(!player(12)->on_ice && player(12)->plus_minus == 0);
	scoreboard_roster_from_string("10:0:0:-4:-4:0:-4:-4");
	assert(player(10)->goals == 0 && player(10)->assists == 0);
	assert(player(10)->season_goals == 0 && player(10)->season_assists == 0);
}

static void test_roster_string_skips_bad_entries_and_bounds(void)
{
	setup_roster();
	scoreboard_roster_from_string(
		"abc,,7:1:3:junk,-5:0:0,1000:0:0, 8:1,9:x,4294967299:0:0");
	assert(scoreboard_roster_find(7));
	assert(scoreboard_roster_find(8));
	assert(scoreboard_roster_find(9));
	assert(!scoreboard_roster_find(1000));
	assert(!scoreboard_roster_find(-5));

	scoreboard_roster_from_string("5:0:1,5:1:2");
	assert(scoreboard_roster_count() == 1);

	char big[1024] = "";
	for (int i = 1; i <= SCOREBOARD_MAX_ROSTER + 5; i++) {
		char item[16];
		snprintf(item, sizeof(item), "%s%d", i > 1 ? "," : "", i);
		strcat(big, item);
	}
	scoreboard_roster_from_string(big);
	assert(scoreboard_roster_count() == SCOREBOARD_MAX_ROSTER);

	scoreboard_roster_from_string(NULL);
	assert(scoreboard_roster_count() == 0);
	scoreboard_roster_from_string("");
	assert(scoreboard_roster_count() == 0);
}

/* ---- saved game state ---- */

static void test_save_and_load_round_trip(void)
{
	char path[512];
	setup_tmp_dir();
	setup_roster();
	scoreboard_set_plus_minus_skip_power_play(false);
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	scoreboard_player_adjust_plus_minus(12, 4);
	scoreboard_player_set_goals(11, 2);
	scoreboard_player_set_assists(11, 3);
	scoreboard_player_set_season(11, 6, 20, 30);

	snprintf(path, sizeof(path), "%s/state.json", g_tmp_dir);
	assert(scoreboard_save_state(path));
	scoreboard_reset_state_for_tests();
	assert(scoreboard_get_plus_minus_skip_power_play());
	assert(scoreboard_load_state(path));

	assert(!scoreboard_get_plus_minus_skip_power_play());
	assert(scoreboard_roster_count() == 3);
	assert(player(10)->on_ice && player(10)->plus_minus == 1);
	assert(player(12)->plus_minus == 4 && player(12)->season_plus_minus == 4);
	assert(player(11)->goals == 2 && player(11)->assists == 3);
	assert(player(11)->season_plus_minus == 6);
	assert(player(11)->season_goals == 20);
	assert(player(11)->season_assists == 30);
	/* the goal history is not saved */
	scoreboard_decrement_home_score();
	assert(player(10)->plus_minus == 1);
	cleanup_tmp_dir();
}

static void test_load_older_state_files(void)
{
	char path[512];
	FILE *f;
	setup_tmp_dir();
	setup_roster();
	scoreboard_player_adjust_plus_minus(10, 2);

	snprintf(path, sizeof(path), "%s/old.json", g_tmp_dir);
	f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_score\": 4\n}\n");
	fclose(f);
	assert(scoreboard_load_state(path));
	assert(scoreboard_get_home_score() == 4);
	assert(scoreboard_roster_count() == 3);
	assert(player(10)->plus_minus == 2);
	assert(scoreboard_get_plus_minus_skip_power_play());

	/* a file with a roster but no season numbers (and old away entries) */
	f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_roster_count\": 1,\n"
		   "  \"home_player0_number\": 7,\n"
		   "  \"home_player0_plus_minus\": 3,\n"
		   "  \"home_player0_goals\": 2,\n"
		   "  \"home_player0_assists\": 1,\n"
		   "  \"away_roster_count\": 1,\n"
		   "  \"away_player0_number\": 9\n}\n");
	fclose(f);
	assert(scoreboard_load_state(path));
	assert(scoreboard_roster_count() == 1);
	assert(player(7)->season_plus_minus == 3);
	assert(player(7)->season_goals == 2 && player(7)->season_assists == 1);

	/* a huge count is clamped */
	f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_roster_count\": 500,\n"
		   "  \"home_player0_number\": 7\n}\n");
	fclose(f);
	assert(scoreboard_load_state(path));
	assert(scoreboard_roster_count() == SCOREBOARD_MAX_ROSTER);
	assert(scoreboard_roster_get(0)->number == 7);
	cleanup_tmp_dir();
}

/* ---- who was on the ice ---- */

static void test_set_goal_on_ice_moves_plus_minus(void)
{
	int now[2] = {11, 12};
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	assert(player(10)->plus_minus == 1);

	assert(scoreboard_set_goal_on_ice(true, now, 2));
	assert(player(10)->plus_minus == 0 && !player(10)->on_ice);
	assert(player(11)->plus_minus == 1 && player(11)->on_ice);
	assert(player(12)->plus_minus == 1 && player(12)->season_plus_minus == 1);

	/* taking the goal back reverses the corrected players */
	scoreboard_decrement_home_score();
	assert(player(11)->plus_minus == 0 && player(12)->plus_minus == 0);
	assert(player(10)->plus_minus == 0);
}

static void test_set_goal_on_ice_for_away_goal(void)
{
	int now[1] = {10};
	setup_roster();
	scoreboard_player_set_on_ice(12, true);
	scoreboard_increment_away_score();
	assert(player(12)->plus_minus == -1);
	assert(scoreboard_set_goal_on_ice(false, now, 1));
	assert(player(12)->plus_minus == 0);
	assert(player(10)->plus_minus == -1);
	/* a home goal in between is left alone */
	scoreboard_increment_home_score();
	assert(player(10)->plus_minus == 0);
	/* the away goal had nobody on ice after all: only the home goal's +1 stays */
	assert(scoreboard_set_goal_on_ice(false, now, 0));
	assert(player(10)->plus_minus == 1 && !player(10)->on_ice);
}

static void test_set_goal_on_ice_nobody(void)
{
	setup_roster();
	scoreboard_player_set_on_ice(10, true);
	scoreboard_increment_home_score();
	assert(scoreboard_set_goal_on_ice(true, NULL, 0));
	assert(player(10)->plus_minus == 0);
	assert(scoreboard_roster_on_ice_count() == 0);
}

static void test_set_goal_on_ice_keeps_penalty_rule(void)
{
	int now[1] = {10};
	setup_roster();
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_home_score();
	assert(scoreboard_set_goal_on_ice(true, now, 1));
	assert(player(10)->on_ice && player(10)->plus_minus == 0);
	scoreboard_decrement_home_score();
	assert(player(10)->plus_minus == 0);
}

static void test_set_goal_on_ice_without_goal_and_unknown_player(void)
{
	int now[2] = {10, 99};
	setup_roster();
	assert(!scoreboard_set_goal_on_ice(true, now, 2));
	assert(player(10)->on_ice);
	assert(scoreboard_roster_on_ice_count() == 1);
	assert(player(10)->plus_minus == 0);

	/* a player removed from the roster after the goal is skipped */
	scoreboard_increment_home_score();
	scoreboard_roster_remove(10);
	int again[1] = {11};
	assert(scoreboard_set_goal_on_ice(true, again, 1));
	assert(player(11)->plus_minus == 1);
}

int main(void)
{
	test_roster_add_get_remove_clear();
	test_roster_full();
	test_roster_marks_dirty();
	test_on_ice();

	test_home_goal_gives_plus_one_to_on_ice();
	test_away_goal_gives_minus_one_to_on_ice();
	test_goal_with_nobody_on_ice_or_no_roster();
	test_no_plus_minus_while_a_penalty_is_active();
	test_power_play_goals_count_when_skip_disabled();
	test_goal_adds_action_log_entry();

	test_decrement_reverses_goal_for_original_players();
	test_decrement_reverses_away_goal();
	test_decrement_reverses_most_recent_goal_first();
	test_decrement_skips_other_teams_goals();
	test_decrement_edge_cases();
	test_set_score_forgets_goals_only_on_change();
	test_goal_history_is_bounded();

	test_manual_game_edits_move_season_too();
	test_set_season_directly();
	test_season_goal_edit_never_below_zero();
	test_reset_game_and_season();
	test_new_game_keeps_season_and_roster();

	test_credit_goal();
	test_credit_goal_optional_parts();
	test_credit_goal_rejects_bad_input();
	test_credit_goal_correction_replaces_credit();
	test_credit_goal_goes_to_latest_home_goal();
	test_removing_goal_removes_credit();
	test_removing_goal_never_goes_below_zero();
	test_credit_goal_without_recorded_goal();
	test_last_goal_and_format();

	test_set_goal_on_ice_moves_plus_minus();
	test_set_goal_on_ice_for_away_goal();
	test_set_goal_on_ice_nobody();
	test_set_goal_on_ice_keeps_penalty_rule();
	test_set_goal_on_ice_without_goal_and_unknown_player();

	test_format_plus_minus_values();
	test_plus_minus_lines();
	test_scoring_lines();
	test_files_written();
	test_files_empty_without_roster();

	test_roster_string_round_trip();
	test_roster_string_truncates_cleanly();
	test_roster_string_older_forms_load();
	test_roster_string_skips_bad_entries_and_bounds();

	test_save_and_load_round_trip();
	test_load_older_state_files();

	printf("All plus/minus tests passed\n");
	return 0;
}
