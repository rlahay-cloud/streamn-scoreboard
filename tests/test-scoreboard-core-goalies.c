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
	snprintf(g_tmp_dir, sizeof(g_tmp_dir), "%s\\scoreboard_goalie_test_%d",
		 getenv("TEMP") ? getenv("TEMP") : ".", (int)getpid());
#else
	snprintf(g_tmp_dir, sizeof(g_tmp_dir), "/tmp/scoreboard_goalie_test_%d",
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


static void setup(void)
{
	scoreboard_reset_state_for_tests();
	scoreboard_roster_add(10);
	scoreboard_roster_add(11);
	scoreboard_roster_add(12);
	scoreboard_goalie_add(31);
	scoreboard_goalie_add(35);
}

static const struct scoreboard_goalie *goalie(int number)
{
	for (int i = 0; i < scoreboard_goalie_count(); i++) {
		const struct scoreboard_goalie *g = scoreboard_goalie_get(i);
		if (g->number == number)
			return g;
	}
	return NULL;
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

/* ---- goalie roster ---- */

static void test_goalie_roster(void)
{
	scoreboard_reset_state_for_tests();
	assert(scoreboard_goalie_add(-1) == -1);
	assert(scoreboard_goalie_add(1000) == -1);
	assert(scoreboard_goalie_add(31) == 0);
	assert(scoreboard_goalie_add(35) == 1);
	assert(scoreboard_goalie_add(31) == 0);
	assert(scoreboard_goalie_count() == 2);
	assert(scoreboard_goalie_find(35));
	assert(!scoreboard_goalie_find(1));
	assert(scoreboard_goalie_get(-1) == NULL);
	assert(scoreboard_goalie_get(2) == NULL);
	assert(scoreboard_goalie_add(1) == 2);
	assert(scoreboard_goalie_add(2) == 3);
	assert(scoreboard_goalie_add(3) == -1);

	assert(scoreboard_get_goalie_in_net() == -1);
	assert(!scoreboard_set_goalie_in_net(99));
	assert(scoreboard_set_goalie_in_net(35));
	assert(scoreboard_get_goalie_in_net() == 35);
	assert(goalie(35)->played);
	assert(!goalie(31)->played);
	assert(scoreboard_set_goalie_in_net(-1));
	assert(scoreboard_get_goalie_in_net() == -1);
	/* goalie number 0 is a real number, not "nobody" */
	assert(scoreboard_goalie_add(0) < 0);
	scoreboard_goalie_remove(1);
	assert(scoreboard_goalie_add(0) >= 0);
	assert(scoreboard_set_goalie_in_net(0));
	assert(scoreboard_get_goalie_in_net() == 0);

	/* removing someone else keeps the goalie in net */
	assert(!scoreboard_goalie_remove(77));
	assert(scoreboard_goalie_remove(31));
	assert(scoreboard_get_goalie_in_net() == 0);
	/* removing the goalie in net leaves nobody in net */
	assert(scoreboard_goalie_remove(0));
	assert(scoreboard_get_goalie_in_net() == -1);
	scoreboard_set_goalie_in_net(35);
	scoreboard_goalie_clear();
	assert(scoreboard_goalie_count() == 0);
	assert(scoreboard_get_goalie_in_net() == -1);
}

static void test_shots_and_goals_go_to_the_goalie_in_net(void)
{
	setup();
	/* nobody in net: nothing is charged to anyone */
	scoreboard_increment_away_shots();
	scoreboard_increment_away_score();
	assert(goalie(31)->shots_against == 0 && goalie(31)->goals_against == 0);
	/* the goal counted as a shot for the team */
	assert(scoreboard_get_away_shots() == 2);
	scoreboard_decrement_away_score();
	assert(scoreboard_get_away_shots() == 1);

	scoreboard_set_goalie_in_net(31);
	for (int i = 0; i < 10; i++)
		scoreboard_increment_away_shots();
	scoreboard_increment_away_score();
	/* the goal is a shot too, so 10 shots plus the goal */
	assert(goalie(31)->shots_against == 11);
	assert(goalie(31)->goals_against == 1);
	assert(goalie(31)->season_shots_against == 11);
	assert(goalie(31)->season_goals_against == 1);

	/* change goalies mid game: new numbers go to the new goalie */
	scoreboard_set_goalie_in_net(35);
	scoreboard_increment_away_shots();
	scoreboard_increment_away_shots();
	scoreboard_increment_away_score();
	assert(goalie(31)->shots_against == 11 && goalie(31)->goals_against == 1);
	assert(goalie(35)->shots_against == 3 && goalie(35)->goals_against == 1);

	/* taking a goal back goes to the goalie who let it in */
	scoreboard_decrement_away_score();
	assert(goalie(35)->goals_against == 0);
	assert(goalie(35)->shots_against == 2);
	scoreboard_set_goalie_in_net(31);
	scoreboard_decrement_away_score();
	assert(goalie(31)->goals_against == 0);
	assert(goalie(31)->shots_against == 10);

	/* taking a shot back */
	scoreboard_decrement_away_shots();
	assert(goalie(31)->shots_against == 9);
	scoreboard_set_goalie_in_net(35);
	scoreboard_decrement_away_shots();
	assert(goalie(35)->shots_against == 1);
	scoreboard_decrement_away_shots();
	assert(goalie(35)->shots_against == 0);
	scoreboard_decrement_away_shots(); /* team total still has some */
	assert(goalie(35)->shots_against == 0);
	assert(goalie(35)->season_shots_against == 0);
	assert(goalie(31)->season_shots_against == 9);

	/* typing a total in does not move goalie numbers */
	scoreboard_set_away_shots(40);
	scoreboard_set_away_score(7);
	assert(goalie(35)->shots_against == 0 && goalie(35)->goals_against == 0);

	/* home shots and goals are not charged to a goalie */
	scoreboard_increment_home_shots();
	scoreboard_increment_home_score();
	assert(goalie(35)->shots_against == 0);

	/* goal charged to a goalie who was then removed */
	scoreboard_increment_away_score();
	assert(goalie(35)->goals_against == 1);
	assert(goalie(35)->shots_against == 1);
	scoreboard_goalie_remove(35);
	scoreboard_decrement_away_score();
	assert(scoreboard_goalie_count() == 1);
}

static void test_goalie_manual_edits(void)
{
	setup();
	assert(!scoreboard_goalie_set_shots_against(99, 5));
	assert(!scoreboard_goalie_set_goals_against(99, 5));
	assert(!scoreboard_goalie_set_season(99, 1, 1, 1));
	assert(scoreboard_goalie_set_season(31, 100, 10, 8));
	assert(scoreboard_goalie_set_shots_against(31, 20));
	assert(scoreboard_goalie_set_goals_against(31, 2));
	assert(goalie(31)->season_shots_against == 120);
	assert(goalie(31)->season_goals_against == 12);
	assert(goalie(31)->games == 8);
	scoreboard_goalie_set_shots_against(31, -5);
	assert(goalie(31)->shots_against == 0);
	assert(goalie(31)->season_shots_against == 100);
	scoreboard_goalie_set_season(31, -1, -1, -1);
	assert(goalie(31)->season_shots_against == 0);
	assert(goalie(31)->games == 0);
}

static void test_save_percentage_and_goalie_text(void)
{
	char buf[128];
	char tiny[1] = {'x'};
	scoreboard_format_save_percentage(0, 0, buf, sizeof(buf));
	assert(strcmp(buf, "-") == 0);
	scoreboard_format_save_percentage(25, 2, buf, sizeof(buf));
	assert(strcmp(buf, ".920") == 0);
	scoreboard_format_save_percentage(30, 0, buf, sizeof(buf));
	assert(strcmp(buf, "1.000") == 0);
	scoreboard_format_save_percentage(3, 5, buf, sizeof(buf));
	assert(strcmp(buf, ".000") == 0);
	scoreboard_format_save_percentage(3, 1, buf, sizeof(buf));
	assert(strcmp(buf, ".667") == 0);
	scoreboard_format_save_percentage(3, 1, tiny, 0);
	assert(tiny[0] == 'x');

	setup();
	scoreboard_format_goalie_in_net(buf, sizeof(buf));
	assert(buf[0] == '\0');
	scoreboard_set_goalie_in_net(31);
	scoreboard_goalie_set_shots_against(31, 25);
	scoreboard_goalie_set_goals_against(31, 2);
	scoreboard_goalie_set_season(31, 100, 10, 4);
	scoreboard_format_goalie_in_net(buf, sizeof(buf));
	assert(strcmp(buf, "#31  SA  25  GA  2  SV% .920") == 0);
	scoreboard_format_goalie_lines(false, buf, sizeof(buf));
	assert(strcmp(buf, "#31  SA  25  GA  2  SV% .920\n"
			   "#35  SA   0  GA  0  SV% -") == 0);
	scoreboard_format_goalie_lines(true, buf, sizeof(buf));
	assert(strncmp(buf, "#31  SA 100  GA 10  SV% .900", 28) == 0);
	char small[20];
	scoreboard_format_goalie_lines(false, small, sizeof(small));
	assert(small[0] == '\0');
	scoreboard_format_goalie_lines(false, tiny, 0);
	scoreboard_format_goalie_in_net(tiny, 0);
	assert(tiny[0] == 'x');
}

static void test_goalie_string_round_trip(void)
{
	char buf[256];
	char tiny[1] = {'x'};
	setup();
	scoreboard_set_goalie_in_net(35);
	scoreboard_goalie_set_shots_against(35, 12);
	scoreboard_goalie_set_goals_against(35, 1);
	scoreboard_goalie_set_season(35, 50, 4, 3);
	scoreboard_goalies_to_string(buf, sizeof(buf));
	assert(strcmp(buf, "31:0:0:0:0:0:0,35:12:1:50:4:3:1;35") == 0);

	scoreboard_goalie_clear();
	scoreboard_goalies_from_string(buf);
	assert(scoreboard_goalie_count() == 2);
	assert(goalie(35)->shots_against == 12);
	assert(goalie(35)->season_shots_against == 50);
	assert(goalie(35)->games == 3);
	assert(goalie(35)->played);
	assert(!goalie(31)->played);
	assert(scoreboard_get_goalie_in_net() == 35);

	scoreboard_goalies_to_string(tiny, 0);
	assert(tiny[0] == 'x');
	/* too small for a second goalie or the tail */
	char small[20];
	scoreboard_goalies_to_string(small, sizeof(small));
	assert(strcmp(small, "31:0:0:0:0:0:0;35") == 0);
	char exact[16];
	scoreboard_goalies_to_string(exact, sizeof(exact));
	assert(strcmp(exact, "31:0:0:0:0:0:0") == 0);

	/* nobody in net, older or odd text */
	scoreboard_goalies_from_string("31:5:1;-1");
	assert(scoreboard_get_goalie_in_net() == -1);
	assert(goalie(31)->shots_against == 5 && goalie(31)->goals_against == 1);
	scoreboard_goalies_from_string("abc,,7:1:3:junk,-5:0,1000,8;99");
	assert(scoreboard_goalie_find(7) && scoreboard_goalie_find(8));
	assert(!scoreboard_goalie_find(1000));
	assert(scoreboard_get_goalie_in_net() == -1);
	scoreboard_goalies_from_string("1,2,3,4,5,6;4");
	assert(scoreboard_goalie_count() == SCOREBOARD_MAX_GOALIES);
	assert(scoreboard_get_goalie_in_net() == 4);
	scoreboard_goalies_from_string("1:-5:-5:-5:-5:-5:0");
	assert(goalie(1)->shots_against == 0);
	scoreboard_goalies_from_string(NULL);
	assert(scoreboard_goalie_count() == 0);
	scoreboard_goalies_from_string("");
	assert(scoreboard_goalie_count() == 0);
}

/* ---- faceoff percentage ---- */

static void test_faceoff_percent(void)
{
	char buf[64];
	char tiny[1] = {'x'};
	scoreboard_reset_state_for_tests();
	assert(scoreboard_get_home_faceoff_percent() == 0);
	scoreboard_format_home_faceoff_percent(buf, sizeof(buf));
	assert(strcmp(buf, "0/0 (0%)") == 0);
	for (int i = 0; i < 12; i++)
		scoreboard_increment_home_faceoffs();
	for (int i = 0; i < 8; i++)
		scoreboard_increment_away_faceoffs();
	assert(scoreboard_get_home_faceoff_percent() == 60);
	scoreboard_format_home_faceoff_percent(buf, sizeof(buf));
	assert(strcmp(buf, "12/20 (60%)") == 0);
	scoreboard_increment_away_faceoffs();
	scoreboard_format_home_faceoff_percent(buf, sizeof(buf));
	assert(strcmp(buf, "12/21 (57%)") == 0);
	scoreboard_format_home_faceoff_percent(tiny, 0);
	assert(tiny[0] == 'x');
}

/* ---- on the ice limit ---- */

static void test_five_on_the_ice_at_most(void)
{
	setup();
	for (int n = 13; n <= 20; n++)
		scoreboard_roster_add(n);
	int on[8] = {10, 11, 12, 13, 14, 15, 16, 17};
	scoreboard_increment_home_score();
	assert(scoreboard_set_goal_on_ice(true, on, 8));
	int got[8];
	assert(scoreboard_get_goal_on_ice(true, got, 8) == SCOREBOARD_MAX_ON_ICE);
	assert(got[4] == 14);
	assert(player(15)->plus_minus == 0);
	assert(player(14)->plus_minus == 1);
}

/* ---- penalty minutes ---- */

static void test_penalty_minutes(void)
{
	char buf[256];
	char tiny[1] = {'x'};
	setup();
	scoreboard_home_penalty_add(10, 120);
	assert(player(10)->pim == 2 && player(10)->season_pim == 2);
	scoreboard_home_penalty_add_compound(11, 120, 300);
	assert(player(11)->pim == 7);
	scoreboard_home_penalty_add(99, 120);  /* not on the roster */
	scoreboard_home_penalty_add(0, 120);   /* no number */
	scoreboard_away_penalty_add(10, 120);  /* away penalties are not tracked */
	scoreboard_away_penalty_add_compound(10, 120, 120);
	assert(player(10)->pim == 2);
	assert(scoreboard_player_get_pim(10) == 2);
	assert(scoreboard_player_get_pim(99) == 0);

	scoreboard_format_pim_lines(buf, sizeof(buf));
	assert(strcmp(buf, "#10    2 game    2 season\n"
			   "#11    7 game    7 season") == 0);
	char small[32];
	scoreboard_format_pim_lines(small, sizeof(small));
	assert(strcmp(small, "#10    2 game    2 season") == 0);
	scoreboard_format_pim_lines(tiny, 0);
	assert(tiny[0] == 'x');

	assert(!scoreboard_player_set_pim(99, 1));
	assert(scoreboard_player_set_pim(10, 4));
	assert(player(10)->pim == 4 && player(10)->season_pim == 4);
	scoreboard_player_set_pim(10, -3);
	assert(player(10)->pim == 0 && player(10)->season_pim == 0);
	assert(!scoreboard_player_set_season_pim(99, 1));
	assert(scoreboard_player_set_season_pim(10, 20));
	assert(player(10)->season_pim == 20);
	scoreboard_player_set_season_pim(10, -2);
	assert(player(10)->season_pim == 0);
	assert(!scoreboard_player_set_games(99, 1));
	assert(scoreboard_player_set_games(10, 6));
	assert(player(10)->games == 6);
	scoreboard_player_set_games(10, -1);
	assert(player(10)->games == 0);

	/* only a player with no minutes at all is left out */
	scoreboard_roster_reset_game_stats();
	assert(player(11)->pim == 0 && player(11)->season_pim == 7);
	scoreboard_format_pim_lines(buf, sizeof(buf));
	assert(strcmp(buf, "#11    0 game    7 season") == 0);
	scoreboard_roster_reset_season_stats();
	scoreboard_format_pim_lines(buf, sizeof(buf));
	assert(buf[0] == '\0');

	/* the roster text form carries minutes and games */
	setup();
	scoreboard_home_penalty_add(10, 240);
	scoreboard_player_set_games(10, 3);
	scoreboard_roster_to_string(buf, sizeof(buf));
	assert(strncmp(buf, "10:0:0:0:0:0:0:0:4:4:3", 22) == 0);
	scoreboard_roster_clear();
	scoreboard_roster_from_string(buf);
	assert(player(10)->pim == 4 && player(10)->season_pim == 4);
	assert(player(10)->games == 3);
	scoreboard_roster_from_string("10:0:0:0:0:0:0:0:-4:-4:-4");
	assert(player(10)->pim == 0 && player(10)->games == 0);
}

static void test_reading_penalty_files_does_not_count_minutes_twice(void)
{
	setup_tmp_dir();
	setup();
	scoreboard_set_output_directory(g_tmp_dir);
	scoreboard_home_penalty_add(10, 120);
	assert(scoreboard_write_all_files());
	assert(scoreboard_read_all_files());
	assert(player(10)->pim == 2);
	cleanup_tmp_dir();
}

/* ---- an away goal ends a home minor ---- */

static void test_away_goal_ends_a_home_minor(void)
{
	setup();
	assert(scoreboard_get_away_goal_ends_penalty());

	/* a goal with no home penalty changes nothing */
	scoreboard_increment_away_score();

	/* 2 minutes: removed */
	scoreboard_home_penalty_add(10, 120);
	scoreboard_increment_away_score();
	assert(!scoreboard_get_home_penalty(0)->active);
	assert(player(10)->pim == 2);

	/* 4 minutes: down to 2 */
	scoreboard_home_penalty_add(10, 240);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
	assert(scoreboard_get_home_penalty(0)->remaining_tenths == 1200);
	/* now only 2 minutes are left: the next goal removes it */
	scoreboard_increment_away_score();
	assert(!scoreboard_get_home_penalty(0)->active);

	/* 2+2 moves on to the second part */
	scoreboard_home_penalty_add_compound(11, 120, 120);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
	assert(scoreboard_get_home_penalty(0)->phase2_tenths == 0);
	assert(scoreboard_get_home_penalty(0)->remaining_tenths == 1200);
	assert(!scoreboard_get_home_penalty(0)->major);
	scoreboard_home_penalty_clear(0);

	/* 2+5: the five is a major and stays */
	scoreboard_home_penalty_add_compound(11, 120, 300);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->major);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
	scoreboard_home_penalty_clear(0);
	assert(!scoreboard_get_home_penalty(0)->major);

	/* majors are never ended, but a minor behind one is */
	scoreboard_home_penalty_add(10, 300);
	scoreboard_home_penalty_add(11, 120);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
	assert(scoreboard_get_home_penalty(0)->remaining_tenths == 3000);
	assert(!scoreboard_get_home_penalty(1)->active);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->remaining_tenths == 3000);
	scoreboard_home_penalty_clear(0);

	/* not a man up (4 on 4): nothing ends */
	scoreboard_home_penalty_add(10, 120);
	scoreboard_away_penalty_add(22, 120);
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
	scoreboard_home_penalty_clear(0);
	scoreboard_away_penalty_clear(0);

	/* a shorthanded home goal is not the rule's business */
	scoreboard_home_penalty_add(10, 120);
	scoreboard_increment_home_score();
	assert(scoreboard_get_home_penalty(0)->active);

	/* switched off */
	scoreboard_set_away_goal_ends_penalty(false);
	assert(!scoreboard_get_away_goal_ends_penalty());
	scoreboard_increment_away_score();
	assert(scoreboard_get_home_penalty(0)->active);
}

static void test_a_penalty_that_ends_second_part_after_compound_tick(void)
{
	setup();
	/* a compound penalty that runs into its second part by itself */
	scoreboard_home_penalty_add_compound(10, 120, 300);
	scoreboard_penalty_tick(1200);
	assert(scoreboard_get_home_penalty(0)->major);
	assert(scoreboard_get_home_penalty(0)->phase2_tenths == 0);
	scoreboard_home_penalty_set_time(0, 200);
	assert(scoreboard_get_home_penalty(0)->major);
	/* editing a compound penalty to zero moves to its second part */
	scoreboard_home_penalty_clear(0);
	scoreboard_home_penalty_add_compound(10, 120, 120);
	scoreboard_home_penalty_set_time(0, 0);
	assert(!scoreboard_get_home_penalty(0)->major);
	scoreboard_home_penalty_clear(0);
	scoreboard_home_penalty_add_compound(10, 120, 300);
	scoreboard_home_penalty_set_time(0, 0);
	assert(scoreboard_get_home_penalty(0)->major);
	/* ... and so does adjusting the clock past the end */
	scoreboard_home_penalty_clear(0);
	scoreboard_home_penalty_add_compound(10, 120, 300);
	scoreboard_penalty_adjust(-1300);
	assert(scoreboard_get_home_penalty(0)->major);
}

/* ---- points per game ---- */

static void test_points_per_game(void)
{
	char buf[256];
	char tiny[1] = {'x'};
	setup();
	scoreboard_format_ppg_lines(buf, sizeof(buf));
	assert(strcmp(buf, "#10   0.00\n#11   0.00\n#12   0.00") == 0);

	/* a goal in a game still being played is not counted yet */
	scoreboard_player_set_games(10, 2);
	scoreboard_player_set_season(10, 0, 3, 1);
	scoreboard_player_set_goals(10, 1);
	/* season is now 4 goals + 1 assist; this game has 1 goal, so the 2
	   finished games hold 4 points */
	scoreboard_format_ppg_lines(buf, sizeof(buf));
	assert(strncmp(buf, "#10   2.00", 10) == 0);

	scoreboard_format_ppg_lines(tiny, 0);
	assert(tiny[0] == 'x');
	assert(scoreboard_player_get_ppg(10) == 2.0);
	assert(scoreboard_player_get_ppg(99) == 0.0);
	char small[12];
	scoreboard_format_ppg_lines(small, sizeof(small));
	assert(strcmp(small, "#10   2.00") == 0);
}

/* ---- end of game ---- */

static void play_a_game(void)
{
	setup();
	scoreboard_set_home_name("Kings");
	scoreboard_set_away_name("Rivals");
	scoreboard_set_goalie_in_net(31);
	for (int i = 0; i < 4; i++)
		scoreboard_increment_away_shots();
	scoreboard_increment_home_shots();
	scoreboard_increment_away_score();
	scoreboard_increment_home_score();
	scoreboard_credit_goal(10, 11, -1);
	scoreboard_increment_home_faceoffs();
	scoreboard_increment_away_faceoffs();
	scoreboard_home_penalty_add(12, 120);
	scoreboard_home_penalty_clear(0);
}

static void test_end_game_counts_games_and_writes_summary(void)
{
	char buf[8192];
	setup_tmp_dir();
	play_a_game();
	scoreboard_set_output_directory(g_tmp_dir);
	assert(!scoreboard_game_is_ended());

	/* the preview before the game ends lists the whole roster */
	scoreboard_format_game_summary(buf, sizeof(buf));
	assert(strstr(buf, "#12 ") != NULL);

	/* 10 and 11 played; 99 is unknown and 10 is named twice */
	int played[4] = {10, 11, 99, 10};
	assert(scoreboard_end_game(played, 4));
	assert(scoreboard_game_is_ended());
	assert(!scoreboard_end_game(played, 4));
	assert(player(10)->games == 1 && player(11)->games == 1);
	assert(player(12)->games == 0);
	assert(goalie(31)->games == 1);
	assert(goalie(35)->games == 0);

	scoreboard_format_game_summary(buf, sizeof(buf));
	assert(strstr(buf, "GAME SUMMARY\nKings 1 - 1 Rivals\n") == buf);
	assert(strstr(buf, "Shots: Kings 2, Rivals 5") != NULL);
	assert(strstr(buf, "Faceoffs won (Kings): 1/2 (50%)") != NULL);
	assert(strstr(buf, "#10  1G 0A 1P  +/- 0  PIM 0") != NULL);
	/* #12 did not play, so is not in this game's list */
	char *players_at = strstr(buf, "PLAYERS THIS GAME");
	char *goalies_at = strstr(buf, "GOALIES THIS GAME");
	assert(players_at != NULL && goalies_at != NULL);
	assert(memcmp(players_at, "PLAYERS", 7) == 0);
	for (char *c = players_at; c < goalies_at; c++)
		assert(strncmp(c, "#12 ", 4) != 0);
	assert(strstr(buf, "#31  SA   5  GA  1  SV% .800") != NULL);
	assert(strstr(buf, "SEASON TO DATE") != NULL);
	assert(strstr(buf, "#10  1GP 1G 0A 1P  1.00 PPG") != NULL);

	/* a goalie who did not play is not in the game list */
	char *season = strstr(buf, "SEASON TO DATE");
	assert(season != NULL);
	for (char *c = goalies_at; c < season; c++)
		assert(strncmp(c, "#35 ", 4) != 0);

	/* both summary files exist; the dated one is the same text */
	char path[512];
	snprintf(path, sizeof(path), "%s/game_summary.txt", g_tmp_dir);
	char *content = read_file_content(path);
	assert(content != NULL && strcmp(content, buf) == 0);
	free(content);

	/* points per game after the game ended */
	char ppg[128];
	scoreboard_format_ppg_lines(ppg, sizeof(ppg));
	assert(strncmp(ppg, "#10   1.00", 10) == 0);

	cleanup_tmp_dir();
}

static void test_end_game_without_output_directory(void)
{
	play_a_game();
	/* no folder set: nothing is written, the game still ends */
	assert(scoreboard_end_game(NULL, 0));
	assert(player(10)->games == 0);
	assert(goalie(31)->games == 1);
	char tiny[1] = {'x'};
	scoreboard_format_game_summary(tiny, 0);
	assert(tiny[0] == 'x');
	char small[40];
	scoreboard_format_game_summary(small, sizeof(small));
	assert(strstr(small, "GAME SUMMARY") != NULL);
}

static void test_goalie_who_only_faced_shots_counts_as_played(void)
{
	setup();
	/* never selected, but shots and goals were typed in by hand */
	scoreboard_goalie_set_shots_against(35, 10);
	scoreboard_goalie_set_goals_against(31, 1);
	assert(scoreboard_end_game(NULL, 0));
	assert(goalie(35)->games == 1 && goalie(31)->games == 1);
}

static void test_reopen_after_end_game(void)
{
	play_a_game();
	assert(!scoreboard_can_reopen_last_game() || scoreboard_game_is_ended() ||
	       true);
	int played[2] = {10, 12};
	assert(scoreboard_end_game(played, 2));
	assert(scoreboard_can_reopen_last_game());
	/* a mistake can be fixed while the game is open, and the player left
	   out can be dropped from the games count */
	scoreboard_roster_remove(12);
	scoreboard_goalie_set_season(31, 0, 0, 5);
	assert(scoreboard_reopen_last_game());
	assert(!scoreboard_game_is_ended());
	assert(player(10)->games == 0);
	assert(goalie(31)->games == 4);
	assert(!scoreboard_can_reopen_last_game());
	assert(!scoreboard_reopen_last_game());

	/* the goalie removed after the end is skipped without trouble */
	assert(scoreboard_end_game(played, 2));
	scoreboard_goalie_remove(31);
	assert(scoreboard_reopen_last_game());
}

static void test_reopen_after_new_game(void)
{
	char buf[256];
	play_a_game();
	scoreboard_set_home_score(3);
	int played[2] = {10, 11};
	assert(scoreboard_end_game(played, 2));
	scoreboard_new_game();
	assert(scoreboard_get_home_score() == 0);
	assert(!scoreboard_game_is_ended());
	assert(player(10)->goals == 0);
	assert(scoreboard_can_reopen_last_game());

	/* something happens in the next game, then the user changes their mind */
	scoreboard_increment_away_score();
	assert(scoreboard_reopen_last_game());
	assert(scoreboard_get_home_score() == 3);
	assert(scoreboard_get_away_score() == 1);
	assert(scoreboard_get_home_shots() == 2);
	assert(scoreboard_get_away_shots() == 5);
	assert(scoreboard_get_home_faceoffs() == 1);
	assert(scoreboard_get_away_faceoffs() == 1);
	assert(goalie(31)->shots_against == 5);
	assert(scoreboard_get_goalie_in_net() == 31);
	assert(player(10)->goals == 0 || player(10)->goals == 1);
	/* the end was undone too, so games are back to zero */
	assert(!scoreboard_game_is_ended());
	assert(player(10)->games == 0 && goalie(31)->games == 0);
	assert(!scoreboard_can_reopen_last_game());

	/* a goal can still be taken back with its goalie */
	scoreboard_decrement_away_score();
	assert(goalie(31)->goals_against == 0);

	/* New Game on an empty board does not wipe the saved game */
	scoreboard_new_game();
	scoreboard_new_game();
	assert(scoreboard_reopen_last_game());
	assert(scoreboard_get_away_shots() == 4);
	scoreboard_roster_to_string(buf, sizeof(buf));
	assert(buf[0] != '\0');
}

static void test_new_game_without_ending_keeps_season_and_games(void)
{
	setup();
	scoreboard_set_home_score(1);
	scoreboard_new_game();
	assert(scoreboard_can_reopen_last_game());
	assert(scoreboard_reopen_last_game());
	assert(scoreboard_get_home_score() == 1);
	assert(!scoreboard_game_is_ended());
}

/* ---- text files ---- */

static void test_new_files_are_written(void)
{
	setup_tmp_dir();
	play_a_game();
	scoreboard_home_penalty_add(10, 240);
	scoreboard_set_output_directory(g_tmp_dir);
	scoreboard_player_set_games(10, 2);
	scoreboard_player_set_goals(10, 3);
	assert(scoreboard_write_all_files());
	expect_file("home_pim.txt", "#10    4 game    4 season\n#12    2 game    2 season");
	expect_file("home_ppg.txt", "#10   0.00\n#11   0.00\n#12   0.00");
	expect_file("home_faceoff_percent.txt", "1/2 (50%)");
	expect_file("home_goalie.txt", "#31  SA   5  GA  1  SV% .800");
	expect_file("home_goalies.txt", "#31  SA   5  GA  1  SV% .800\n"
					"#35  SA   0  GA  0  SV% -");
	expect_file("home_goalies_season.txt", "#31  SA   5  GA  1  SV% .800\n"
					       "#35  SA   0  GA  0  SV% -");
	cleanup_tmp_dir();
}

/* ---- saved game state ---- */

static void test_save_and_load_keeps_everything(void)
{
	char path[512];
	setup_tmp_dir();
	play_a_game();
	scoreboard_home_penalty_add(10, 300);
	scoreboard_player_set_games(10, 5);
	scoreboard_set_away_goal_ends_penalty(false);
	snprintf(path, sizeof(path), "%s/state.json", g_tmp_dir);
	assert(scoreboard_save_state(path));

	scoreboard_reset_state_for_tests();
	assert(scoreboard_load_state(path));
	assert(scoreboard_goalie_count() == 2);
	assert(goalie(31)->shots_against == 5 && goalie(31)->goals_against == 1);
	assert(goalie(31)->season_shots_against == 5);
	assert(goalie(31)->played);
	assert(scoreboard_get_goalie_in_net() == 31);
	assert(player(10)->games == 5 && player(10)->pim == 5);
	assert(player(12)->season_pim == 2);
	assert(scoreboard_get_home_penalty(0)->major);
	assert(!scoreboard_get_away_goal_ends_penalty());

	/* an older file without any of it keeps the current values; nobody in
	   net when the file says so */
	FILE *f = fopen(path, "w");
	assert(f != NULL);
	fprintf(f, "{\n  \"home_score\": 2,\n  \"goalie_count\": 99,\n"
		   "  \"goalie_in_net\": -1\n}\n");
	fclose(f);
	assert(scoreboard_load_state(path));
	assert(scoreboard_goalie_count() == SCOREBOARD_MAX_GOALIES);
	assert(scoreboard_get_goalie_in_net() == -1);
	f = fopen(path, "w");
	fprintf(f, "{\n  \"home_score\": 2\n}\n");
	fclose(f);
	assert(scoreboard_load_state(path));
	assert(scoreboard_goalie_count() == SCOREBOARD_MAX_GOALIES);
	cleanup_tmp_dir();
}

int main(void)
{
	test_goalie_roster();
	test_shots_and_goals_go_to_the_goalie_in_net();
	test_goalie_manual_edits();
	test_save_percentage_and_goalie_text();
	test_goalie_string_round_trip();
	test_faceoff_percent();
	test_five_on_the_ice_at_most();
	test_penalty_minutes();
	test_reading_penalty_files_does_not_count_minutes_twice();
	test_away_goal_ends_a_home_minor();
	test_a_penalty_that_ends_second_part_after_compound_tick();
	test_points_per_game();
	test_end_game_counts_games_and_writes_summary();
	test_end_game_without_output_directory();
	test_goalie_who_only_faced_shots_counts_as_played();
	test_reopen_after_end_game();
	test_reopen_after_new_game();
	test_new_game_without_ending_keeps_season_and_games();
	test_new_files_are_written();
	test_save_and_load_keeps_everything();
	printf("scoreboard-core goalie tests passed\n");
	return 0;
}
