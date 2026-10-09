#include "scoreboard-core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCOREBOARD_MAX_NAME 65
#define SCOREBOARD_MAX_PATH 512
#define SCOREBOARD_ACTION_LOG_CAPACITY 64
#define SCOREBOARD_ACTION_LOG_ENTRY_SIZE 160
#define SCOREBOARD_DEFAULT_PERIOD_LENGTH 900
#define SCOREBOARD_DEFAULT_PENALTY_DURATION 120
#define SCOREBOARD_DEFAULT_MAJOR_PENALTY_DURATION 300
#define SCOREBOARD_PENALTY_SLOTS SCOREBOARD_MAX_PENALTIES
#define SCOREBOARD_SEGMENT_NAME_SIZE 16
#define SCOREBOARD_PM_EVENT_CAPACITY 16
#define SCOREBOARD_MAJOR_SECS 300

/* One goal's plus/minus awards, kept so a goal that is taken back can be
   reversed. Players are recorded by jersey number. */
struct pm_event {
	bool home_scored;
	int scorer;  /* credited jersey numbers (home goals only), -1 when nobody */
	int assist1;
	int assist2;
	bool skipped; /* no +/- because a penalty was active */
	int count;   /* home players who got +1 (home goal) or -1 (away goal) */
	int goalie;  /* goalie in net for an away goal (-1 none) */
	int numbers[SCOREBOARD_MAX_ROSTER];
};

static const struct scoreboard_sport_preset k_sport_presets[SCOREBOARD_SPORT_COUNT] = {
	/* sport, segment_name, segment_count, duration_seconds, ot_max, has_shots, has_faceoffs, has_penalties, default_direction, has_fouls, foul_label, foul_label2, log_scores, score_label, default_penalty_secs, default_major_penalty_secs, base_strength, min_strength */
	{SCOREBOARD_SPORT_HOCKEY,     "Period",  3, 900,  4, true,  true,  true,  SCOREBOARD_CLOCK_COUNT_DOWN, false, "",      "", true,  "Goal",  120, 300, 5,  3},
	{SCOREBOARD_SPORT_BASKETBALL, "Quarter", 4, 480,  1, false, false, false, SCOREBOARD_CLOCK_COUNT_DOWN, true,  "Fouls", "", false, "Score", 0,   0,   0,  0},
	{SCOREBOARD_SPORT_SOCCER,     "Half",    2, 2700, 1, false, false, false, SCOREBOARD_CLOCK_COUNT_UP,   true,  "YC",    "RC", true,  "Goal",  0,   0,   11, 7},
	{SCOREBOARD_SPORT_FOOTBALL,   "Half",    2, 1800, 1, false, false, false, SCOREBOARD_CLOCK_COUNT_DOWN, true,  "Flags", "", false, "Score", 0,   0,   0,  0},
	{SCOREBOARD_SPORT_LACROSSE,   "Quarter", 4, 720,  1, true,  true,  true,  SCOREBOARD_CLOCK_COUNT_DOWN, false, "",      "", true,  "Goal",  60,  180, 5,  3},
	{SCOREBOARD_SPORT_RUGBY,      "Half",    2, 2400, 1, false, false, true,  SCOREBOARD_CLOCK_COUNT_UP,   false, "",      "", true,  "Try",   120, 600, 15, 13},
	{SCOREBOARD_SPORT_GENERIC,    "Segment", 1, 0,    0, false, false, false, SCOREBOARD_CLOCK_COUNT_UP,   false, "",      "", true,  "Score", 120, 300, 0,  0},
};

static struct {
	int clock_tenths;
	bool clock_running;
	enum scoreboard_clock_direction clock_direction;
	int period_length;

	bool game_clock_enabled;
	int game_clock_accumulated_tenths;
	bool game_clock_started;
	enum scoreboard_game_clock_format game_clock_display_format;

	int period;
	bool overtime_enabled;
	int default_penalty_duration;
	int default_major_penalty_duration;

	enum scoreboard_sport sport;
	char segment_name[SCOREBOARD_SEGMENT_NAME_SIZE];
	int segment_count;
	int ot_max;
	bool has_shots;
	bool has_penalties;
	int base_strength;
	int min_strength;

	char period_labels[SCOREBOARD_MAX_PERIOD_LABELS]
			  [SCOREBOARD_PERIOD_LABEL_SIZE];
	int period_label_count;

	char home_name[SCOREBOARD_MAX_NAME];
	char away_name[SCOREBOARD_MAX_NAME];

	int home_score;
	int away_score;
	int home_shots;
	int away_shots;
	int home_faceoffs;
	int away_faceoffs;
	bool has_faceoffs;
	int home_fouls;
	int away_fouls;
	bool has_fouls;
	char foul_label[16];
	int home_fouls2;
	int away_fouls2;
	char foul_label2[16];
	bool log_scores;
	char score_label[16];

	struct scoreboard_penalty home_penalties[SCOREBOARD_PENALTY_SLOTS];
	struct scoreboard_penalty away_penalties[SCOREBOARD_PENALTY_SLOTS];

	char penalty_label_format[SCOREBOARD_PENALTY_LABEL_FORMAT_SIZE];
	char strength_label_format[SCOREBOARD_STRENGTH_LABEL_FORMAT_SIZE];

	struct scoreboard_player home_roster[SCOREBOARD_MAX_ROSTER];
	int home_roster_count;
	bool pm_skip_power_play;
	struct pm_event pm_events[SCOREBOARD_PM_EVENT_CAPACITY];
	int pm_event_count;

	struct scoreboard_goalie goalies[SCOREBOARD_MAX_GOALIES];
	int goalie_count;
	bool has_goalie_in_net;
	int goalie_in_net;
	bool away_goal_ends_penalty;
	bool game_ended;
	int ended_players[SCOREBOARD_MAX_ROSTER];
	int ended_player_count;
	int ended_goalies[SCOREBOARD_MAX_GOALIES];
	int ended_goalie_count;

	char output_directory[SCOREBOARD_MAX_PATH];

	char cli_executable[SCOREBOARD_MAX_PATH];
	char cli_extra_args[SCOREBOARD_MAX_PATH];

	scoreboard_log_fn log_fn;

	char action_logs[SCOREBOARD_ACTION_LOG_CAPACITY]
			[SCOREBOARD_ACTION_LOG_ENTRY_SIZE];
	int action_log_head;
	int action_log_count;
} g_state;

/* What the last New Game wiped, so it can be brought back. */
static struct {
	bool valid;
	int home_score, away_score;
	int home_shots, away_shots;
	int home_faceoffs, away_faceoffs;
	struct scoreboard_player roster[SCOREBOARD_MAX_ROSTER];
	int roster_count;
	struct scoreboard_goalie goalies[SCOREBOARD_MAX_GOALIES];
	int goalie_count;
	bool has_goalie_in_net;
	int goalie_in_net;
	struct pm_event pm_events[SCOREBOARD_PM_EVENT_CAPACITY];
	int pm_event_count;
	bool game_ended;
	int ended_players[SCOREBOARD_MAX_ROSTER];
	int ended_player_count;
	int ended_goalies[SCOREBOARD_MAX_GOALIES];
	int ended_goalie_count;
} g_prev_game;

static bool g_dirty;

static const char *kDefaultPenaltyLabelFormat =
	"#{{ number }}  {{ time }}{{ if_phase2 }} (+{{ phase2 }}){{ end_if }}";

static const char *kDefaultStrengthLabelFormat = "{{ home }}-{{ away }}";

/* ---- game event log ---- */
static struct scoreboard_game_event
	g_event_log[SCOREBOARD_MAX_EVENTS];
static int g_event_count;

/* ---- helpers ---- */

static void mark_dirty(void)
{
	g_dirty = true;
}

bool scoreboard_is_dirty(void)
{
	return g_dirty;
}

void scoreboard_mark_dirty(void)
{
	g_dirty = true;
}

static void safe_copy(char *dst, const char *src, size_t dst_size)
{
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	size_t len = strlen(src);
	if (len >= dst_size)
		len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void log_message(enum scoreboard_log_level level, const char *msg)
{
	if (g_state.log_fn != NULL)
		g_state.log_fn(level, msg);
}

static void generate_default_period_labels(void);

static bool read_text_file(const char *dir, const char *filename, char *buf,
			   size_t buf_size)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, filename);
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 0 || (size_t)size >= buf_size)
		size = (long)(buf_size - 1);
	size_t n = fread(buf, 1, (size_t)size, f);
	buf[n] = '\0';
	fclose(f);
	return true;
}

static int parse_clock_text(const char *text)
{
	int minutes = 0, seconds = 0;
	if (sscanf(text, "%d:%d", &minutes, &seconds) == 2)
		return (minutes * 60 + seconds) * 10;
	return -1;
}

static int parse_cumulative_clock_text(const char *text)
{
	int a = 0, b = 0, c = 0;
	if (sscanf(text, "%d:%d:%d", &a, &b, &c) == 3)
		return (a * 3600 + b * 60 + c) * 10; /* H:MM:SS */
	if (sscanf(text, "%d:%d", &a, &b) == 2)
		return (a * 60 + b) * 10; /* M:SS */
	return -1;
}

static int parse_period_text(const char *text)
{
	/* Search labels array for an exact match */
	for (int i = 0; i < g_state.period_label_count; i++) {
		if (strcmp(text, g_state.period_labels[i]) == 0)
			return i + 1;
	}
	/* Fallback: try parsing as a number */
	int p = 0;
	if (sscanf(text, "%d", &p) == 1 && p >= 1 &&
	    p <= g_state.period_label_count)
		return p;
	return -1;
}

/* A compound penalty moves to its second part. The second part is a major
   if it is 5 minutes or more. */
static void penalty_phase_two(struct scoreboard_penalty *p)
{
	p->major = p->phase2_tenths >= 3000;
	p->phase2_tenths = 0;
}

/* Set while penalties are re-read from text files so that adding them back
   does not count penalty minutes a second time. */
static bool g_loading_penalties;

static void parse_penalty_files(const char *numbers_text,
				const char *times_text, bool home)
{
	/* Save phase2_tenths before clearing — text files cannot carry
	   compound phase info, so we preserve it for matching penalties */
	struct scoreboard_penalty *penalties =
		home ? g_state.home_penalties : g_state.away_penalties;
	int saved_phase2[SCOREBOARD_PENALTY_SLOTS];
	int saved_player[SCOREBOARD_PENALTY_SLOTS];
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		saved_phase2[i] = penalties[i].phase2_tenths;
		saved_player[i] = penalties[i].player_number;
	}

	g_loading_penalties = true;

	/* Clear all existing penalties for this team */
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (home)
			scoreboard_home_penalty_clear(i);
		else
			scoreboard_away_penalty_clear(i);
	}

	if (numbers_text[0] == '\0' || times_text[0] == '\0') {
		g_loading_penalties = false;
		return;
	}

	/* Walk both texts line by line in parallel */
	const char *np = numbers_text;
	const char *tp = times_text;
	while (*np != '\0' && *tp != '\0') {
		/* Extract player number from current line */
		int player = 0;
		const char *nl = strchr(np, '\n');
		size_t nlen = nl ? (size_t)(nl - np) : strlen(np);
		char nline[32];
		if (nlen >= sizeof(nline))
			nlen = sizeof(nline) - 1;
		memcpy(nline, np, nlen);
		nline[nlen] = '\0';
		if (nline[0] == '#')
			sscanf(nline + 1, "%d", &player);

		/* Extract time from current line */
		const char *tl = strchr(tp, '\n');
		size_t tlen = tl ? (size_t)(tl - tp) : strlen(tp);
		char tline[32];
		if (tlen >= sizeof(tline))
			tlen = sizeof(tline) - 1;
		memcpy(tline, tp, tlen);
		tline[tlen] = '\0';
		int minutes = 0, seconds = 0;
		if (sscanf(tline, "%d:%d", &minutes, &seconds) == 2) {
			int duration_secs = minutes * 60 + seconds;
			if (duration_secs > 0) {
				int slot;
				if (home)
					slot = scoreboard_home_penalty_add(
						player, duration_secs);
				else
					slot = scoreboard_away_penalty_add(
						player, duration_secs);
				/* Restore phase2_tenths if the same player
				   had compound data before the re-parse */
				if (slot >= 0) {
					for (int j = 0;
					     j < SCOREBOARD_PENALTY_SLOTS;
					     j++) {
						if (saved_player[j] ==
							    player &&
						    saved_phase2[j] > 0) {
							penalties[slot]
								.phase2_tenths =
								saved_phase2[j];
							saved_phase2[j] = 0;
							break;
						}
					}
				}
			}
		}

		np = nl ? nl + 1 : np + nlen;
		tp = tl ? tl + 1 : tp + tlen;
	}
	g_loading_penalties = false;
}

static bool write_text_file(const char *dir, const char *filename,
			    const char *content)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, filename);
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return false;
	fprintf(f, "%s", content);
	fclose(f);
	return true;
}

static const char *find_json_value(const char *json, const char *key)
{
	char pattern[256];
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *pos = strstr(json, pattern);
	if (pos == NULL)
		return NULL;
	pos += strlen(pattern);
	while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' ||
	       *pos == ':')
		pos++;
	return pos;
}

static int parse_json_int(const char *json, const char *key, int default_val)
{
	const char *val = find_json_value(json, key);
	if (val == NULL)
		return default_val;
	return atoi(val);
}

static bool parse_json_bool(const char *json, const char *key, bool default_val)
{
	const char *val = find_json_value(json, key);
	if (val == NULL)
		return default_val;
	if (strncmp(val, "true", 4) == 0)
		return true;
	if (strncmp(val, "false", 5) == 0)
		return false;
	return default_val;
}

static void parse_json_string(const char *json, const char *key, char *out,
			      size_t out_size)
{
	const char *val = find_json_value(json, key);
	if (val == NULL || *val != '"') {
		out[0] = '\0';
		return;
	}
	val++;
	size_t i = 0;
	while (*val != '\0' && *val != '"' && i < out_size - 1) {
		if (*val == '\\' && *(val + 1) != '\0')
			val++;
		out[i++] = *val++;
	}
	out[i] = '\0';
}

static void write_json_string(FILE *f, const char *key, const char *value,
			      bool last)
{
	fprintf(f, "  \"%s\": \"", key);
	for (const char *p = value; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			fputc('\\', f);
		fputc(*p, f);
	}
	fprintf(f, last ? "\"\n" : "\",\n");
}

/* ---- lifecycle ---- */

const char *scoreboard_description(void)
{
	return "Streamn Scoreboard";
}

bool scoreboard_on_load(scoreboard_log_fn log_fn)
{
	g_state.log_fn = log_fn;
	log_message(SCOREBOARD_LOG_INFO,
		    "[streamn-obs-scoreboard] module loaded");
	return true;
}

void scoreboard_on_unload(scoreboard_log_fn log_fn)
{
	if (log_fn != NULL)
		log_fn(SCOREBOARD_LOG_INFO,
		       "[streamn-obs-scoreboard] module unloaded");
	g_state.log_fn = NULL;
}

void scoreboard_reset_state_for_tests(void)
{
	memset(&g_state, 0, sizeof(g_state));
	memset(&g_prev_game, 0, sizeof(g_prev_game));
	g_dirty = false;
	g_event_count = 0;
	memset(g_event_log, 0, sizeof(g_event_log));
	g_state.period = 1;
	g_state.period_length = SCOREBOARD_DEFAULT_PERIOD_LENGTH;
	g_state.clock_direction = SCOREBOARD_CLOCK_COUNT_DOWN;
	g_state.clock_tenths = SCOREBOARD_DEFAULT_PERIOD_LENGTH * 10;
	g_state.overtime_enabled = true;
	g_state.default_penalty_duration = SCOREBOARD_DEFAULT_PENALTY_DURATION;
	g_state.default_major_penalty_duration =
		SCOREBOARD_DEFAULT_MAJOR_PENALTY_DURATION;
	safe_copy(g_state.home_name, "Home", sizeof(g_state.home_name));
	safe_copy(g_state.away_name, "Away", sizeof(g_state.away_name));

	/* Hockey defaults for sport fields */
	g_state.sport = SCOREBOARD_SPORT_HOCKEY;
	safe_copy(g_state.segment_name, "Period",
		  sizeof(g_state.segment_name));
	g_state.segment_count = 3;
	g_state.ot_max = 4;
	g_state.has_shots = true;
	g_state.has_faceoffs = true;
	g_state.has_penalties = true;
	g_state.base_strength = 5;
	g_state.min_strength = 3;
	g_state.has_fouls = false;
	g_state.foul_label[0] = '\0';
	g_state.foul_label2[0] = '\0';
	g_state.log_scores = true;
	g_state.pm_skip_power_play = true;
	g_state.away_goal_ends_penalty = true;
	safe_copy(g_state.score_label, "Goal", sizeof(g_state.score_label));
	safe_copy(g_state.penalty_label_format, kDefaultPenaltyLabelFormat,
		  sizeof(g_state.penalty_label_format));
	safe_copy(g_state.strength_label_format, kDefaultStrengthLabelFormat,
		  sizeof(g_state.strength_label_format));
	generate_default_period_labels();
}

/* ---- player roster and plus/minus ---- */

/* Only the home team's players are tracked. Every stat has a "game" value
   (cleared by New Game) and a "season" value that keeps adding up across
   games. A change to a game stat moves the season stat by the same amount. */

static void pm_record_goal(bool home_scored);
static void pm_undo_goal(bool home_scored);
static void pm_forget_goals(bool home_scored);

static int roster_index(int number)
{
	for (int i = 0; i < g_state.home_roster_count; i++) {
		if (g_state.home_roster[i].number == number)
			return i;
	}
	return -1;
}

static struct scoreboard_player *roster_lookup(int number)
{
	int idx = roster_index(number);
	return idx >= 0 ? &g_state.home_roster[idx] : NULL;
}

int scoreboard_roster_add(int number)
{
	if (number < 0 || number > SCOREBOARD_MAX_PLAYER_NUMBER)
		return -1;
	int existing = roster_index(number);
	if (existing >= 0)
		return existing;
	if (g_state.home_roster_count >= SCOREBOARD_MAX_ROSTER)
		return -1;
	struct scoreboard_player *p =
		&g_state.home_roster[g_state.home_roster_count];
	memset(p, 0, sizeof(*p));
	p->number = number;
	mark_dirty();
	return g_state.home_roster_count++;
}

bool scoreboard_roster_remove(int number)
{
	int idx = roster_index(number);
	if (idx < 0)
		return false;
	for (int i = idx; i < g_state.home_roster_count - 1; i++)
		g_state.home_roster[i] = g_state.home_roster[i + 1];
	g_state.home_roster_count--;
	mark_dirty();
	return true;
}

void scoreboard_roster_clear(void)
{
	g_state.home_roster_count = 0;
	g_state.pm_event_count = 0;
	mark_dirty();
}

int scoreboard_roster_count(void)
{
	return g_state.home_roster_count;
}

const struct scoreboard_player *scoreboard_roster_get(int index)
{
	if (index < 0 || index >= g_state.home_roster_count)
		return NULL;
	return &g_state.home_roster[index];
}

bool scoreboard_roster_find(int number)
{
	return roster_index(number) >= 0;
}

/* Change a game stat and the season stat together. Goals and assists never
   drop below zero in either place. */
static void player_add_plus_minus(struct scoreboard_player *p, int delta)
{
	p->plus_minus += delta;
	p->season_plus_minus += delta;
}

static int clamp_zero(int value)
{
	return value < 0 ? 0 : value;
}

/* ---- goalies ---- */

static int goalie_index(int number)
{
	for (int i = 0; i < g_state.goalie_count; i++) {
		if (g_state.goalies[i].number == number)
			return i;
	}
	return -1;
}

static struct scoreboard_goalie *goalie_lookup(int number)
{
	int idx = goalie_index(number);
	return idx >= 0 ? &g_state.goalies[idx] : NULL;
}

int scoreboard_goalie_add(int number)
{
	if (number < 0 || number > SCOREBOARD_MAX_PLAYER_NUMBER)
		return -1;
	int existing = goalie_index(number);
	if (existing >= 0)
		return existing;
	if (g_state.goalie_count >= SCOREBOARD_MAX_GOALIES)
		return -1;
	struct scoreboard_goalie *g = &g_state.goalies[g_state.goalie_count];
	memset(g, 0, sizeof(*g));
	g->number = number;
	mark_dirty();
	return g_state.goalie_count++;
}

bool scoreboard_goalie_remove(int number)
{
	int idx = goalie_index(number);
	if (idx < 0)
		return false;
	for (int i = idx; i < g_state.goalie_count - 1; i++)
		g_state.goalies[i] = g_state.goalies[i + 1];
	g_state.goalie_count--;
	if (g_state.has_goalie_in_net && g_state.goalie_in_net == number)
		g_state.has_goalie_in_net = false;
	mark_dirty();
	return true;
}

void scoreboard_goalie_clear(void)
{
	g_state.goalie_count = 0;
	g_state.has_goalie_in_net = false;
	mark_dirty();
}

int scoreboard_goalie_count(void)
{
	return g_state.goalie_count;
}

const struct scoreboard_goalie *scoreboard_goalie_get(int index)
{
	if (index < 0 || index >= g_state.goalie_count)
		return NULL;
	return &g_state.goalies[index];
}

bool scoreboard_goalie_find(int number)
{
	return goalie_index(number) >= 0;
}

bool scoreboard_set_goalie_in_net(int number)
{
	if (number < 0) {
		g_state.has_goalie_in_net = false;
		mark_dirty();
		return true;
	}
	struct scoreboard_goalie *g = goalie_lookup(number);
	if (g == NULL)
		return false;
	g->played = true;
	g_state.has_goalie_in_net = true;
	g_state.goalie_in_net = number;
	mark_dirty();
	return true;
}

int scoreboard_get_goalie_in_net(void)
{
	return g_state.has_goalie_in_net ? g_state.goalie_in_net : -1;
}

/* One more (delta = 1) or one fewer (delta = -1) shot against the goalie in
   net. Nothing happens with nobody in net. */
static void goalie_add_shot(int delta)
{
	if (!g_state.has_goalie_in_net)
		return;
	struct scoreboard_goalie *g = goalie_lookup(g_state.goalie_in_net);
	if (delta < 0 && g->shots_against == 0)
		return;
	g->shots_against += delta;
	g->season_shots_against = clamp_zero(g->season_shots_against + delta);
}

/* A goal against a goalie; returns who it was charged to (-1 for nobody). */
static int goalie_add_goal_against(void)
{
	if (!g_state.has_goalie_in_net)
		return -1;
	struct scoreboard_goalie *g = goalie_lookup(g_state.goalie_in_net);
	g->goals_against++;
	g->season_goals_against++;
	return g->number;
}

static void goalie_take_back_goal(int number)
{
	struct scoreboard_goalie *g = goalie_lookup(number);
	if (g == NULL)
		return;
	g->goals_against = clamp_zero(g->goals_against - 1);
	g->season_goals_against = clamp_zero(g->season_goals_against - 1);
}

bool scoreboard_goalie_set_shots_against(int number, int value)
{
	struct scoreboard_goalie *g = goalie_lookup(number);
	if (g == NULL)
		return false;
	int v = clamp_zero(value);
	g->season_shots_against =
		clamp_zero(g->season_shots_against + v - g->shots_against);
	g->shots_against = v;
	mark_dirty();
	return true;
}

bool scoreboard_goalie_set_goals_against(int number, int value)
{
	struct scoreboard_goalie *g = goalie_lookup(number);
	if (g == NULL)
		return false;
	int v = clamp_zero(value);
	g->season_goals_against =
		clamp_zero(g->season_goals_against + v - g->goals_against);
	g->goals_against = v;
	mark_dirty();
	return true;
}

bool scoreboard_goalie_set_season(int number, int sa, int ga, int games)
{
	struct scoreboard_goalie *g = goalie_lookup(number);
	if (g == NULL)
		return false;
	g->season_shots_against = clamp_zero(sa);
	g->season_goals_against = clamp_zero(ga);
	g->games = clamp_zero(games);
	mark_dirty();
	return true;
}

/* ---- penalty minutes ---- */

/* Penalty minutes for a home penalty, unless penalties are only being re-read
   from the text files. */
static void pim_add(int player_number, int secs)
{
	if (g_loading_penalties || player_number <= 0)
		return;
	struct scoreboard_player *p = roster_lookup(player_number);
	if (p == NULL)
		return;
	p->pim += secs / 60;
	p->season_pim += secs / 60;
}

bool scoreboard_player_set_pim(int number, int pim)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	int v = clamp_zero(pim);
	p->season_pim = clamp_zero(p->season_pim + v - p->pim);
	p->pim = v;
	mark_dirty();
	return true;
}

int scoreboard_player_get_pim(int number)
{
	const struct scoreboard_player *p = roster_lookup(number);
	return p != NULL ? p->pim : 0;
}

bool scoreboard_player_set_season_pim(int number, int pim)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	p->season_pim = clamp_zero(pim);
	mark_dirty();
	return true;
}

bool scoreboard_player_set_games(int number, int games)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	p->games = clamp_zero(games);
	mark_dirty();
	return true;
}

bool scoreboard_player_adjust_plus_minus(int number, int delta)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	player_add_plus_minus(p, delta);
	mark_dirty();
	return true;
}

bool scoreboard_player_set_plus_minus(int number, int value)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	player_add_plus_minus(p, value - p->plus_minus);
	mark_dirty();
	return true;
}

bool scoreboard_player_set_goals(int number, int goals)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	int value = clamp_zero(goals);
	p->season_goals = clamp_zero(p->season_goals + value - p->goals);
	p->goals = value;
	mark_dirty();
	return true;
}

bool scoreboard_player_set_assists(int number, int assists)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	int value = clamp_zero(assists);
	p->season_assists = clamp_zero(p->season_assists + value - p->assists);
	p->assists = value;
	mark_dirty();
	return true;
}

bool scoreboard_player_set_season(int number, int plus_minus, int goals,
				  int assists)
{
	struct scoreboard_player *p = roster_lookup(number);
	if (p == NULL)
		return false;
	p->season_plus_minus = plus_minus;
	p->season_goals = clamp_zero(goals);
	p->season_assists = clamp_zero(assists);
	mark_dirty();
	return true;
}

int scoreboard_player_get_plus_minus(int number)
{
	const struct scoreboard_player *p = roster_lookup(number);
	return p != NULL ? p->plus_minus : 0;
}

int scoreboard_player_get_goals(int number)
{
	const struct scoreboard_player *p = roster_lookup(number);
	return p != NULL ? p->goals : 0;
}

int scoreboard_player_get_assists(int number)
{
	const struct scoreboard_player *p = roster_lookup(number);
	return p != NULL ? p->assists : 0;
}

/* Starts a new game: game stats go to zero, season stats stay. */
void scoreboard_roster_reset_game_stats(void)
{
	for (int i = 0; i < g_state.home_roster_count; i++) {
		g_state.home_roster[i].plus_minus = 0;
		g_state.home_roster[i].goals = 0;
		g_state.home_roster[i].assists = 0;
		g_state.home_roster[i].pim = 0;
	}
	for (int i = 0; i < g_state.goalie_count; i++) {
		g_state.goalies[i].shots_against = 0;
		g_state.goalies[i].goals_against = 0;
		g_state.goalies[i].played = false;
	}
	/* Recorded goal awards no longer match the totals. */
	g_state.pm_event_count = 0;
	mark_dirty();
}

void scoreboard_roster_reset_season_stats(void)
{
	for (int i = 0; i < g_state.home_roster_count; i++) {
		g_state.home_roster[i].season_plus_minus = 0;
		g_state.home_roster[i].season_goals = 0;
		g_state.home_roster[i].season_assists = 0;
		g_state.home_roster[i].season_pim = 0;
		g_state.home_roster[i].games = 0;
	}
	for (int i = 0; i < g_state.goalie_count; i++) {
		g_state.goalies[i].season_shots_against = 0;
		g_state.goalies[i].season_goals_against = 0;
		g_state.goalies[i].games = 0;
	}
	mark_dirty();
}

void scoreboard_set_plus_minus_skip_power_play(bool skip)
{
	g_state.pm_skip_power_play = skip;
	mark_dirty();
}

bool scoreboard_get_plus_minus_skip_power_play(void)
{
	return g_state.pm_skip_power_play;
}

void scoreboard_format_plus_minus(int value, char *buf, size_t size)
{
	if (value == 0)
		snprintf(buf, size, "0");
	else
		snprintf(buf, size, "%+d", value);
}

/* Append one line (and a separating newline) if it fits. Returns false when
   there is no more room. */
static bool append_line(char *buf, size_t size, size_t *len, const char *line)
{
	size_t line_len = strlen(line);
	size_t need = line_len + (*len > 0 ? 1 : 0);
	if (*len + need >= size)
		return false;
	if (*len > 0)
		buf[(*len)++] = '\n';
	memcpy(buf + *len, line, line_len + 1);
	*len += line_len;
	return true;
}

/* One line per player: "#12  +2" with right-aligned numbers so the columns
   line up in a monospaced font. */
void scoreboard_format_plus_minus_lines(bool season, char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		char pm[16];
		char line[48];
		scoreboard_format_plus_minus(
			season ? p->season_plus_minus : p->plus_minus, pm,
			sizeof(pm));
		snprintf(line, sizeof(line), "#%-3d %4s", p->number, pm);
		if (!append_line(buf, size, &len, line))
			break;
	}
}

/* One "#12   1G  2A  3P" line per player. The game list only has players with
   at least one goal or assist; the season list has the whole roster. */
void scoreboard_format_scoring_lines(bool season, char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		int goals = season ? p->season_goals : p->goals;
		int assists = season ? p->season_assists : p->assists;
		if (!season && goals == 0 && assists == 0)
			continue;
		char line[64];
		snprintf(line, sizeof(line), "#%-3d %2dG %2dA %2dP", p->number,
			 goals, assists, goals + assists);
		if (!append_line(buf, size, &len, line))
			break;
	}
}

void scoreboard_roster_to_string(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		char item[128];
		/* The second slot used to hold an on-ice flag. It is always 0
		   now but kept so older and newer saves read the same way. */
		snprintf(item, sizeof(item), "%d:%d:%d:%d:%d:%d:%d:%d:%d:%d:%d",
			 p->number, 0, p->plus_minus, p->goals,
			 p->assists, p->season_plus_minus, p->season_goals,
			 p->season_assists, p->pim, p->season_pim, p->games);
		size_t item_len = strlen(item);
		size_t need = item_len + (len > 0 ? 1 : 0);
		if (len + need >= size)
			break;
		if (len > 0)
			buf[len++] = ',';
		memcpy(buf + len, item, item_len + 1);
		len += item_len;
	}
}

void scoreboard_roster_from_string(const char *text)
{
	g_state.home_roster_count = 0;
	g_state.pm_event_count = 0;
	mark_dirty();
	if (text == NULL)
		return;
	const char *p = text;
	while (*p != '\0') {
		char *end = NULL;
		long number = strtol(p, &end, 10);
		/* (unused slot), +/-, goals, assists, then season +/-, goals,
		   assists.
		   Older saves have fewer fields. */
		long fields[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
		int read = 0;
		bool readable = (end != p);
		p = end;
		for (; read < 10 && readable && *p == ':'; read++) {
			fields[read] = strtol(p + 1, &end, 10);
			p = end;
		}
		/* Skip anything left over up to the next entry. */
		while (*p != '\0' && *p != ',')
			p++;
		if (*p == ',')
			p++;
		if (readable && number >= 0 &&
		    number <= SCOREBOARD_MAX_PLAYER_NUMBER) {
			int slot = scoreboard_roster_add((int)number);
			if (slot >= 0) {
				struct scoreboard_player *player =
					&g_state.home_roster[slot];
				player->plus_minus = (int)fields[1];
				player->goals = clamp_zero((int)fields[2]);
				player->assists = clamp_zero((int)fields[3]);
				if (read >= 7) {
					player->season_plus_minus = (int)fields[4];
					player->season_goals =
						clamp_zero((int)fields[5]);
					player->season_assists =
						clamp_zero((int)fields[6]);
				} else {
					/* No season numbers saved yet: start
					   the season from this game. */
					player->season_plus_minus =
						player->plus_minus;
					player->season_goals = player->goals;
					player->season_assists =
						player->assists;
				}
				if (read >= 10) {
					player->pim = clamp_zero((int)fields[7]);
					player->season_pim =
						clamp_zero((int)fields[8]);
					player->games = clamp_zero((int)fields[9]);
				}
			}
		}
	}
}

/* No plus/minus is given for any goal scored while a penalty is active on
   either team (when the "skip" setting is on, which is the default). */
static bool pm_penalty_active(void)
{
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (g_state.home_penalties[i].active ||
		    g_state.away_penalties[i].active)
			return true;
	}
	return false;
}

static void pm_push_event(const struct pm_event *ev)
{
	if (g_state.pm_event_count == SCOREBOARD_PM_EVENT_CAPACITY) {
		memmove(&g_state.pm_events[0], &g_state.pm_events[1],
			sizeof(g_state.pm_events[0]) *
				(SCOREBOARD_PM_EVENT_CAPACITY - 1));
		g_state.pm_event_count--;
	}
	g_state.pm_events[g_state.pm_event_count++] = *ev;
}

/* Add (delta = 1) or take away (delta = -1) one goal and up to two assists.
   Numbers below zero mean nobody. */
static void credit_apply(int scorer, int assist1, int assist2, int delta)
{
	struct scoreboard_player *p = roster_lookup(scorer);
	if (p != NULL) {
		p->goals = clamp_zero(p->goals + delta);
		p->season_goals = clamp_zero(p->season_goals + delta);
	}
	int assists[2] = {assist1, assist2};
	for (int i = 0; i < 2; i++) {
		p = roster_lookup(assists[i]);
		if (p != NULL) {
			p->assists = clamp_zero(p->assists + delta);
			p->season_assists =
				clamp_zero(p->season_assists + delta);
		}
	}
}

static int latest_goal_event(bool home_scored)
{
	for (int i = g_state.pm_event_count - 1; i >= 0; i--) {
		if (g_state.pm_events[i].home_scored == home_scored)
			return i;
	}
	return -1;
}

bool scoreboard_credit_goal(int scorer, int assist1, int assist2)
{
	int given[3] = {scorer, assist1, assist2};
	for (int i = 0; i < 3; i++) {
		if (given[i] < 0)
			continue;
		if (!roster_lookup(given[i]))
			return false;
		for (int j = i + 1; j < 3; j++) {
			if (given[j] == given[i])
				return false;
		}
	}
	int idx = latest_goal_event(true);
	if (idx >= 0) {
		struct pm_event *ev = &g_state.pm_events[idx];
		credit_apply(ev->scorer, ev->assist1, ev->assist2, -1);
		ev->scorer = scorer < 0 ? -1 : scorer;
		ev->assist1 = assist1 < 0 ? -1 : assist1;
		ev->assist2 = assist2 < 0 ? -1 : assist2;
	}
	credit_apply(scorer, assist1, assist2, 1);
	mark_dirty();
	return true;
}

bool scoreboard_get_last_goal(int *scorer, int *assist1, int *assist2)
{
	for (int i = g_state.pm_event_count - 1; i >= 0; i--) {
		const struct pm_event *ev = &g_state.pm_events[i];
		if (ev->scorer < 0)
			continue;
		*scorer = ev->scorer;
		*assist1 = ev->assist1;
		*assist2 = ev->assist2;
		return true;
	}
	return false;
}

bool scoreboard_set_goal_on_ice(bool home_scored, const int *numbers,
				int count)
{
	int idx = latest_goal_event(home_scored);
	if (idx < 0)
		return false;
	struct pm_event *ev = &g_state.pm_events[idx];
	const int delta = home_scored ? 1 : -1;
	for (int i = 0; i < ev->count; i++) {
		struct scoreboard_player *p = roster_lookup(ev->numbers[i]);
		if (p != NULL)
			player_add_plus_minus(p, -delta);
	}
	ev->count = 0;
	if (!ev->skipped) {
		for (int i = 0; i < count; i++) {
			if (ev->count >= SCOREBOARD_MAX_ON_ICE)
				break;
			struct scoreboard_player *p = roster_lookup(numbers[i]);
			bool seen = false;
			for (int j = 0; j < ev->count; j++) {
				if (ev->numbers[j] == numbers[i])
					seen = true;
			}
			if (p == NULL || seen)
				continue;
			player_add_plus_minus(p, delta);
			ev->numbers[ev->count++] = p->number;
		}
		if (ev->count > 0) {
			char msg[SCOREBOARD_ACTION_LOG_ENTRY_SIZE];
			snprintf(msg, sizeof(msg),
				 "Plus/minus: %+d for %d on-ice players", delta,
				 ev->count);
			scoreboard_add_action_log(msg);
		}
	}
	mark_dirty();
	return true;
}

int scoreboard_get_goal_on_ice(bool home_scored, int *numbers, int max)
{
	int idx = latest_goal_event(home_scored);
	if (idx < 0)
		return 0;
	const struct pm_event *ev = &g_state.pm_events[idx];
	int n = ev->count < max ? ev->count : max;
	for (int i = 0; i < n; i++)
		numbers[i] = ev->numbers[i];
	return n;
}

bool scoreboard_goal_has_no_plus_minus(bool home_scored)
{
	int idx = latest_goal_event(home_scored);
	return idx >= 0 && g_state.pm_events[idx].skipped;
}

void scoreboard_format_last_goal(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	int scorer = 0;
	int a1 = 0;
	int a2 = 0;
	if (!scoreboard_get_last_goal(&scorer, &a1, &a2))
		return;
	const char *team = scoreboard_get_home_name();
	if (a1 < 0 && a2 < 0)
		snprintf(buf, size, "%s goal: #%d (unassisted)", team, scorer);
	else if (a1 >= 0 && a2 >= 0)
		snprintf(buf, size, "%s goal: #%d (assists: #%d, #%d)", team,
			 scorer, a1, a2);
	else
		snprintf(buf, size, "%s goal: #%d (assist: #%d)", team, scorer,
			 a1 >= 0 ? a1 : a2);
}

/* A goal is remembered so plus/minus can be given to the players who were on
   the ice (scoreboard_set_goal_on_ice) and reversed if the goal is taken
   back. Nobody gets plus/minus until the on-ice players are named. */
static void pm_record_goal(bool home_scored)
{
	struct pm_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.home_scored = home_scored;
	ev.scorer = -1;
	ev.assist1 = -1;
	ev.assist2 = -1;
	ev.skipped = g_state.pm_skip_power_play && pm_penalty_active();
	ev.goalie = -1;
	if (!home_scored)
		ev.goalie = goalie_add_goal_against();
	pm_push_event(&ev);
}

static void pm_undo_goal(bool home_scored)
{
	for (int i = g_state.pm_event_count - 1; i >= 0; i--) {
		struct pm_event *ev = &g_state.pm_events[i];
		if (ev->home_scored != home_scored)
			continue;
		credit_apply(ev->scorer, ev->assist1, ev->assist2, -1);
		if (ev->goalie >= 0)
			goalie_take_back_goal(ev->goalie);
		int delta = home_scored ? -1 : 1;
		for (int j = 0; j < ev->count; j++) {
			struct scoreboard_player *p =
				roster_lookup(ev->numbers[j]);
			if (p != NULL)
				player_add_plus_minus(p, delta);
		}
		memmove(&g_state.pm_events[i], &g_state.pm_events[i + 1],
			sizeof(g_state.pm_events[0]) *
				(size_t)(g_state.pm_event_count - i - 1));
		g_state.pm_event_count--;
		return;
	}
}

/* The score was typed in directly, so earlier goals can no longer be matched
   up with it and are not reversed later. */
static void pm_forget_goals(bool home_scored)
{
	int kept = 0;
	for (int i = 0; i < g_state.pm_event_count; i++) {
		if (g_state.pm_events[i].home_scored == home_scored)
			continue;
		g_state.pm_events[kept++] = g_state.pm_events[i];
	}
	g_state.pm_event_count = kept;
}

/* ---- away goal ends a home minor penalty ---- */

void scoreboard_set_away_goal_ends_penalty(bool enabled)
{
	g_state.away_goal_ends_penalty = enabled;
	mark_dirty();
}

bool scoreboard_get_away_goal_ends_penalty(void)
{
	return g_state.away_goal_ends_penalty;
}

/* The away team just scored. If the home team is short-handed, the first
   running home minor ends: a 2 minute penalty is removed, a longer minor
   (such as 4 minutes) loses 2 minutes, and a 2+2 moves on to its second part.
   Majors are never ended. */
static void release_home_minor_for_goal(void)
{
	if (!g_state.away_goal_ends_penalty)
		return;
	int home_running = 0;
	int away_running = 0;
	for (int i = 0; i < SCOREBOARD_MAX_RUNNING_PENALTIES; i++) {
		if (g_state.home_penalties[i].active)
			home_running++;
		if (g_state.away_penalties[i].active)
			away_running++;
	}
	if (home_running <= away_running)
		return;
	for (int i = 0; i < SCOREBOARD_MAX_RUNNING_PENALTIES; i++) {
		struct scoreboard_penalty *p = &g_state.home_penalties[i];
		if (!p->active || p->major)
			continue;
		if (p->phase2_tenths > 0) {
			p->remaining_tenths = p->phase2_tenths;
			penalty_phase_two(p);
		} else if (p->remaining_tenths > 1200) {
			p->remaining_tenths -= 1200;
		} else {
			scoreboard_home_penalty_clear(i);
			scoreboard_penalty_compact();
		}
		scoreboard_add_action_log(
			"Away goal ended a home minor penalty");
		return;
	}
}

/* ---- faceoff percentage ---- */

int scoreboard_get_home_faceoff_percent(void)
{
	int total = g_state.home_faceoffs + g_state.away_faceoffs;
	if (total == 0)
		return 0;
	return (int)((g_state.home_faceoffs * 100.0) / total + 0.5);
}

void scoreboard_format_home_faceoff_percent(char *buf, size_t size)
{
	if (size == 0)
		return;
	snprintf(buf, size, "%d/%d (%d%%)", g_state.home_faceoffs,
		 g_state.home_faceoffs + g_state.away_faceoffs,
		 scoreboard_get_home_faceoff_percent());
}

/* ---- goalie text ---- */

void scoreboard_format_save_percentage(int shots_against, int goals_against,
				       char *buf, size_t size)
{
	if (size == 0)
		return;
	if (shots_against <= 0) {
		snprintf(buf, size, "-");
		return;
	}
	double sv = (double)(shots_against - goals_against) / shots_against;
	if (sv < 0)
		sv = 0;
	int permille = (int)(sv * 1000 + 0.5);
	if (permille >= 1000)
		snprintf(buf, size, "1.000");
	else
		snprintf(buf, size, ".%03d", permille);
}

static void goalie_line(const struct scoreboard_goalie *g, bool season,
			char *buf, size_t size)
{
	int sa = season ? g->season_shots_against : g->shots_against;
	int ga = season ? g->season_goals_against : g->goals_against;
	char sv[16];
	scoreboard_format_save_percentage(sa, ga, sv, sizeof(sv));
	snprintf(buf, size, "#%-3d SA %3d  GA %2d  SV%% %s", g->number, sa, ga,
		 sv);
}

void scoreboard_format_goalie_lines(bool season, char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.goalie_count; i++) {
		char line[96];
		goalie_line(&g_state.goalies[i], season, line, sizeof(line));
		if (!append_line(buf, size, &len, line))
			break;
	}
}

void scoreboard_format_goalie_in_net(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	if (!g_state.has_goalie_in_net)
		return;
	goalie_line(goalie_lookup(g_state.goalie_in_net), false, buf, size);
}

void scoreboard_goalies_to_string(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.goalie_count; i++) {
		const struct scoreboard_goalie *g = &g_state.goalies[i];
		char item[96];
		snprintf(item, sizeof(item), "%d:%d:%d:%d:%d:%d:%d", g->number,
			 g->shots_against, g->goals_against,
			 g->season_shots_against, g->season_goals_against,
			 g->games, g->played ? 1 : 0);
		size_t item_len = strlen(item);
		size_t need = item_len + (len > 0 ? 1 : 0);
		if (len + need >= size)
			break;
		if (len > 0)
			buf[len++] = ',';
		memcpy(buf + len, item, item_len + 1);
		len += item_len;
	}
	char tail[24];
	snprintf(tail, sizeof(tail), ";%d", scoreboard_get_goalie_in_net());
	if (len + strlen(tail) < size)
		memcpy(buf + len, tail, strlen(tail) + 1);
}

void scoreboard_goalies_from_string(const char *text)
{
	g_state.goalie_count = 0;
	g_state.has_goalie_in_net = false;
	mark_dirty();
	if (text == NULL)
		return;
	const char *p = text;
	while (*p != '\0' && *p != ';') {
		char *end = NULL;
		long number = strtol(p, &end, 10);
		long fields[6] = {0, 0, 0, 0, 0, 0};
		int read = 0;
		bool readable = (end != p);
		p = end;
		for (; read < 6 && readable && *p == ':'; read++) {
			fields[read] = strtol(p + 1, &end, 10);
			p = end;
		}
		while (*p != '\0' && *p != ',' && *p != ';')
			p++;
		if (*p == ',')
			p++;
		if (!readable)
			continue;
		int slot = scoreboard_goalie_add((int)number);
		if (slot < 0)
			continue;
		struct scoreboard_goalie *g = &g_state.goalies[slot];
		g->shots_against = clamp_zero((int)fields[0]);
		g->goals_against = clamp_zero((int)fields[1]);
		g->season_shots_against = clamp_zero((int)fields[2]);
		g->season_goals_against = clamp_zero((int)fields[3]);
		g->games = clamp_zero((int)fields[4]);
		g->played = fields[5] != 0;
	}
	if (*p == ';') {
		long in_net = strtol(p + 1, NULL, 10);
		scoreboard_set_goalie_in_net((int)in_net);
	}
}

/* ---- penalty minutes and points per game text ---- */

void scoreboard_format_pim_lines(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		if (p->pim == 0 && p->season_pim == 0)
			continue;
		char line[64];
		snprintf(line, sizeof(line), "#%-3d %3d game %4d season",
			 p->number, p->pim, p->season_pim);
		if (!append_line(buf, size, &len, line))
			break;
	}
}

/* Points over finished games only: a game still in progress is left out of
   the points until End Game, the same way it is left out of the games. */
static double player_ppg(const struct scoreboard_player *p)
{
	if (p->games <= 0)
		return 0.0;
	int points = p->season_goals + p->season_assists;
	if (!g_state.game_ended)
		points -= p->goals + p->assists;
	return (double)clamp_zero(points) / p->games;
}

double scoreboard_player_get_ppg(int number)
{
	const struct scoreboard_player *p = roster_lookup(number);
	return p != NULL ? player_ppg(p) : 0.0;
}

void scoreboard_format_ppg_lines(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		char line[48];
		snprintf(line, sizeof(line), "#%-3d %5.2f", p->number,
			 player_ppg(p));
		if (!append_line(buf, size, &len, line))
			break;
	}
}

/* ---- end of game ---- */

bool scoreboard_game_is_ended(void)
{
	return g_state.game_ended;
}

static bool number_in_list(const int *list, int count, int number)
{
	for (int i = 0; i < count; i++) {
		if (list[i] == number)
			return true;
	}
	return false;
}

static bool player_was_in_game(int number)
{
	if (!g_state.game_ended)
		return true;
	return number_in_list(g_state.ended_players,
			      g_state.ended_player_count, number);
}

void scoreboard_format_game_summary(char *buf, size_t size)
{
	if (size == 0)
		return;
	buf[0] = '\0';
	size_t len = 0;
	char line[160];
	char tmp[64];

	append_line(buf, size, &len, "GAME SUMMARY");
	snprintf(line, sizeof(line), "%s %d - %d %s", g_state.home_name,
		 g_state.home_score, g_state.away_score, g_state.away_name);
	append_line(buf, size, &len, line);
	scoreboard_format_period(tmp, sizeof(tmp));
	snprintf(line, sizeof(line), "Period: %s", tmp);
	append_line(buf, size, &len, line);
	snprintf(line, sizeof(line), "Shots: %s %d, %s %d", g_state.home_name,
		 g_state.home_shots, g_state.away_name, g_state.away_shots);
	append_line(buf, size, &len, line);
	scoreboard_format_home_faceoff_percent(tmp, sizeof(tmp));
	snprintf(line, sizeof(line), "Faceoffs won (%s): %s",
		 g_state.home_name, tmp);
	append_line(buf, size, &len, line);

	append_line(buf, size, &len, "");
	append_line(buf, size, &len, "PLAYERS THIS GAME");
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		if (!player_was_in_game(p->number))
			continue;
		scoreboard_format_plus_minus(p->plus_minus, tmp, sizeof(tmp));
		snprintf(line, sizeof(line),
			 "#%-3d %dG %dA %dP  +/- %s  PIM %d", p->number,
			 p->goals, p->assists, p->goals + p->assists, tmp,
			 p->pim);
		append_line(buf, size, &len, line);
	}

	append_line(buf, size, &len, "");
	append_line(buf, size, &len, "GOALIES THIS GAME");
	for (int i = 0; i < g_state.goalie_count; i++) {
		const struct scoreboard_goalie *g = &g_state.goalies[i];
		if (!g->played && g->shots_against == 0 &&
		    g->goals_against == 0)
			continue;
		goalie_line(g, false, line, sizeof(line));
		append_line(buf, size, &len, line);
	}

	append_line(buf, size, &len, "");
	append_line(buf, size, &len, "SEASON TO DATE");
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		scoreboard_format_plus_minus(p->season_plus_minus, tmp,
					     sizeof(tmp));
		snprintf(line, sizeof(line),
			 "#%-3d %dGP %dG %dA %dP  %.2f PPG  +/- %s  PIM %d",
			 p->number, p->games, p->season_goals,
			 p->season_assists, p->season_goals + p->season_assists,
			 player_ppg(p), tmp, p->season_pim);
		append_line(buf, size, &len, line);
	}
	for (int i = 0; i < g_state.goalie_count; i++) {
		const struct scoreboard_goalie *g = &g_state.goalies[i];
		goalie_line(g, true, line, sizeof(line));
		append_line(buf, size, &len, line);
	}
}

#define SCOREBOARD_SUMMARY_SIZE 8192

static void write_game_summary(void)
{
	const char *dir = g_state.output_directory;
	if (dir[0] == '\0')
		return;
	char text[SCOREBOARD_SUMMARY_SIZE];
	scoreboard_format_game_summary(text, sizeof(text));
	write_text_file(dir, "game_summary.txt", text);
	/* A dated copy, so the next game does not overwrite this one. */
	time_t now = time(NULL);
	struct tm *local = localtime(&now);
	char name[64];
	if (local != NULL &&
	    strftime(name, sizeof(name), "game_summary_%Y-%m-%d_%H%M%S.txt",
		     local) > 0)
		write_text_file(dir, name, text);
}

bool scoreboard_end_game(const int *played, int count)
{
	if (g_state.game_ended)
		return false;
	g_state.ended_player_count = 0;
	g_state.ended_goalie_count = 0;
	for (int i = 0; i < count; i++) {
		struct scoreboard_player *p = roster_lookup(played[i]);
		if (p == NULL || number_in_list(g_state.ended_players,
						g_state.ended_player_count,
						p->number))
			continue;
		p->games++;
		g_state.ended_players[g_state.ended_player_count++] = p->number;
	}
	for (int i = 0; i < g_state.goalie_count; i++) {
		struct scoreboard_goalie *g = &g_state.goalies[i];
		if (!g->played && g->shots_against == 0 &&
		    g->goals_against == 0)
			continue;
		g->games++;
		g_state.ended_goalies[g_state.ended_goalie_count++] = g->number;
	}
	g_state.game_ended = true;
	write_game_summary();
	scoreboard_add_action_log("Game ended");
	mark_dirty();
	return true;
}

static void undo_end_game(void)
{
	for (int i = 0; i < g_state.ended_player_count; i++) {
		struct scoreboard_player *p =
			roster_lookup(g_state.ended_players[i]);
		if (p != NULL)
			p->games = clamp_zero(p->games - 1);
	}
	for (int i = 0; i < g_state.ended_goalie_count; i++) {
		struct scoreboard_goalie *g =
			goalie_lookup(g_state.ended_goalies[i]);
		if (g != NULL)
			g->games = clamp_zero(g->games - 1);
	}
	g_state.ended_player_count = 0;
	g_state.ended_goalie_count = 0;
	g_state.game_ended = false;
}

static void prev_game_save(void)
{
	g_prev_game.valid = true;
	g_prev_game.home_score = g_state.home_score;
	g_prev_game.away_score = g_state.away_score;
	g_prev_game.home_shots = g_state.home_shots;
	g_prev_game.away_shots = g_state.away_shots;
	g_prev_game.home_faceoffs = g_state.home_faceoffs;
	g_prev_game.away_faceoffs = g_state.away_faceoffs;
	memcpy(g_prev_game.roster, g_state.home_roster,
	       sizeof(g_prev_game.roster));
	g_prev_game.roster_count = g_state.home_roster_count;
	memcpy(g_prev_game.goalies, g_state.goalies,
	       sizeof(g_prev_game.goalies));
	g_prev_game.goalie_count = g_state.goalie_count;
	g_prev_game.has_goalie_in_net = g_state.has_goalie_in_net;
	g_prev_game.goalie_in_net = g_state.goalie_in_net;
	memcpy(g_prev_game.pm_events, g_state.pm_events,
	       sizeof(g_prev_game.pm_events));
	g_prev_game.pm_event_count = g_state.pm_event_count;
	g_prev_game.game_ended = g_state.game_ended;
	memcpy(g_prev_game.ended_players, g_state.ended_players,
	       sizeof(g_prev_game.ended_players));
	g_prev_game.ended_player_count = g_state.ended_player_count;
	memcpy(g_prev_game.ended_goalies, g_state.ended_goalies,
	       sizeof(g_prev_game.ended_goalies));
	g_prev_game.ended_goalie_count = g_state.ended_goalie_count;
}

static void prev_game_restore(void)
{
	g_state.home_score = g_prev_game.home_score;
	g_state.away_score = g_prev_game.away_score;
	g_state.home_shots = g_prev_game.home_shots;
	g_state.away_shots = g_prev_game.away_shots;
	g_state.home_faceoffs = g_prev_game.home_faceoffs;
	g_state.away_faceoffs = g_prev_game.away_faceoffs;
	memcpy(g_state.home_roster, g_prev_game.roster,
	       sizeof(g_state.home_roster));
	g_state.home_roster_count = g_prev_game.roster_count;
	memcpy(g_state.goalies, g_prev_game.goalies, sizeof(g_state.goalies));
	g_state.goalie_count = g_prev_game.goalie_count;
	g_state.has_goalie_in_net = g_prev_game.has_goalie_in_net;
	g_state.goalie_in_net = g_prev_game.goalie_in_net;
	memcpy(g_state.pm_events, g_prev_game.pm_events,
	       sizeof(g_state.pm_events));
	g_state.pm_event_count = g_prev_game.pm_event_count;
	g_state.game_ended = g_prev_game.game_ended;
	memcpy(g_state.ended_players, g_prev_game.ended_players,
	       sizeof(g_state.ended_players));
	g_state.ended_player_count = g_prev_game.ended_player_count;
	memcpy(g_state.ended_goalies, g_prev_game.ended_goalies,
	       sizeof(g_state.ended_goalies));
	g_state.ended_goalie_count = g_prev_game.ended_goalie_count;
	g_prev_game.valid = false;
}

bool scoreboard_can_reopen_last_game(void)
{
	return g_prev_game.valid || g_state.game_ended;
}

bool scoreboard_reopen_last_game(void)
{
	if (!scoreboard_can_reopen_last_game())
		return false;
	if (g_prev_game.valid)
		prev_game_restore();
	if (g_state.game_ended)
		undo_end_game();
	scoreboard_add_action_log("Last game reopened");
	mark_dirty();
	return true;
}

/* ---- clock ---- */

void scoreboard_clock_start(void)
{
	g_state.clock_running = true;
	g_state.game_clock_started = true;
	mark_dirty();
}

void scoreboard_clock_stop(void)
{
	g_state.clock_running = false;
	mark_dirty();
}

bool scoreboard_clock_is_running(void)
{
	return g_state.clock_running;
}

void scoreboard_clock_reset(void)
{
	g_state.clock_running = false;
	if (g_state.clock_direction == SCOREBOARD_CLOCK_COUNT_DOWN)
		g_state.clock_tenths = g_state.period_length * 10;
	else
		g_state.clock_tenths = 0;
	mark_dirty();
}

void scoreboard_clock_tick(int elapsed_tenths)
{
	if (!g_state.clock_running)
		return;

	if (g_state.clock_direction == SCOREBOARD_CLOCK_COUNT_DOWN) {
		g_state.clock_tenths -= elapsed_tenths;
		if (g_state.clock_tenths <= 0) {
			g_state.clock_tenths = 0;
			g_state.clock_running = false;
		}
	} else {
		g_state.clock_tenths += elapsed_tenths;
		int max_tenths = g_state.period_length * 10;
		if (g_state.clock_tenths >= max_tenths) {
			g_state.clock_tenths = max_tenths;
			g_state.clock_running = false;
		}
	}

	mark_dirty();
	if (g_state.clock_running)
		scoreboard_penalty_tick(elapsed_tenths);
}

int scoreboard_clock_get_tenths(void)
{
	return g_state.clock_tenths;
}

void scoreboard_clock_set_tenths(int tenths)
{
	if (tenths < 0)
		tenths = 0;
	g_state.clock_tenths = tenths;
	mark_dirty();
}

void scoreboard_clock_adjust_seconds(int delta)
{
	int before = g_state.clock_tenths;
	g_state.clock_tenths += delta * 10;
	if (g_state.clock_tenths < 0)
		g_state.clock_tenths = 0;
	mark_dirty();
	int actual_delta = g_state.clock_tenths - before;
	if (actual_delta != 0)
		scoreboard_penalty_adjust(actual_delta);
}

void scoreboard_clock_adjust_minutes(int delta)
{
	int before = g_state.clock_tenths;
	g_state.clock_tenths += delta * 600;
	if (g_state.clock_tenths < 0)
		g_state.clock_tenths = 0;
	mark_dirty();
	int actual_delta = g_state.clock_tenths - before;
	if (actual_delta != 0)
		scoreboard_penalty_adjust(actual_delta);
}

void scoreboard_clock_format(char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	int total_seconds = g_state.clock_tenths / 10;
	int minutes = total_seconds / 60;
	int seconds = total_seconds % 60;
	snprintf(buf, size, "%d:%02d", minutes, seconds);
}

void scoreboard_set_clock_direction(enum scoreboard_clock_direction dir)
{
	g_state.clock_direction = dir;
	mark_dirty();
}

enum scoreboard_clock_direction scoreboard_get_clock_direction(void)
{
	return g_state.clock_direction;
}

void scoreboard_set_period_length(int seconds)
{
	if (seconds < 1)
		seconds = 1;
	g_state.period_length = seconds;
	mark_dirty();
}

int scoreboard_get_period_length(void)
{
	return g_state.period_length;
}

/* ---- game clock (cumulative) ---- */

static int current_period_elapsed_tenths(void)
{
	/* Derive elapsed from the displayed (truncated-to-seconds) period
	   clock so that displayed_period + cumulative == period_length
	   at every instant.  Without this, independent truncation of raw
	   tenths causes a 1-second drift (GitHub #16). */
	int displayed_seconds = g_state.clock_tenths / 10;
	if (g_state.clock_direction == SCOREBOARD_CLOCK_COUNT_DOWN)
		return (g_state.period_length - displayed_seconds) * 10;
	return displayed_seconds * 10;
}

void scoreboard_set_game_clock_enabled(bool enabled)
{
	g_state.game_clock_enabled = enabled;
	mark_dirty();
}

bool scoreboard_get_game_clock_enabled(void)
{
	return g_state.game_clock_enabled;
}

int scoreboard_game_clock_get_tenths(void)
{
	if (!g_state.game_clock_enabled || !g_state.game_clock_started)
		return 0;
	return g_state.game_clock_accumulated_tenths +
	       current_period_elapsed_tenths();
}

void scoreboard_game_clock_format(char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	int tenths = scoreboard_game_clock_get_tenths();
	int total_seconds = tenths / 10;
	if (g_state.game_clock_display_format ==
	    SCOREBOARD_GAME_CLOCK_FORMAT_HMMSS) {
		int hours = total_seconds / 3600;
		int minutes = (total_seconds % 3600) / 60;
		int seconds = total_seconds % 60;
		if (hours > 0)
			snprintf(buf, size, "%d:%02d:%02d", hours, minutes,
				 seconds);
		else
			snprintf(buf, size, "%d:%02d", minutes, seconds);
	} else {
		int minutes = total_seconds / 60;
		int seconds = total_seconds % 60;
		snprintf(buf, size, "%d:%02d", minutes, seconds);
	}
}

void scoreboard_set_game_clock_display_format(
	enum scoreboard_game_clock_format fmt)
{
	g_state.game_clock_display_format = fmt;
	mark_dirty();
}

enum scoreboard_game_clock_format
scoreboard_get_game_clock_display_format(void)
{
	return g_state.game_clock_display_format;
}

/* ---- period ---- */

int scoreboard_get_period(void)
{
	return g_state.period;
}

void scoreboard_set_period(int period)
{
	if (period < 1)
		period = 1;
	if (period > g_state.period_label_count)
		period = g_state.period_label_count;
	g_state.period = period;
	mark_dirty();
}

void scoreboard_period_advance(void)
{
	if (g_state.period < g_state.period_label_count) {
		if (g_state.game_clock_enabled && g_state.game_clock_started)
			g_state.game_clock_accumulated_tenths +=
				current_period_elapsed_tenths();
		g_state.period++;
		scoreboard_clock_reset();
		mark_dirty();
	}
}

void scoreboard_period_rewind(void)
{
	if (g_state.period > 1) {
		if (g_state.game_clock_enabled && g_state.game_clock_started) {
			g_state.game_clock_accumulated_tenths -=
				g_state.period_length * 10;
			if (g_state.game_clock_accumulated_tenths < 0)
				g_state.game_clock_accumulated_tenths = 0;
		}
		g_state.period--;
		scoreboard_clock_reset();
		mark_dirty();
	}
}

void scoreboard_format_period(char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	int idx = g_state.period - 1;
	if (idx >= 0 && idx < g_state.period_label_count) {
		snprintf(buf, size, "%s", g_state.period_labels[idx]);
		return;
	}
	/* Fallback for periods beyond labels */
	snprintf(buf, size, "%d", g_state.period);
}

void scoreboard_set_overtime_enabled(bool enabled)
{
	g_state.overtime_enabled = enabled;
	generate_default_period_labels();
	if (g_state.period > g_state.period_label_count)
		g_state.period = g_state.period_label_count;
	mark_dirty();
}

bool scoreboard_get_overtime_enabled(void)
{
	return g_state.overtime_enabled;
}

/* ---- period labels ---- */

static void generate_default_period_labels(void)
{
	int count = 0;
	/* Regular segments: "1", "2", "3", ... */
	for (int i = 0; i < g_state.segment_count &&
			count < SCOREBOARD_MAX_PERIOD_LABELS;
	     i++) {
		snprintf(g_state.period_labels[count],
			 SCOREBOARD_PERIOD_LABEL_SIZE, "%d", i + 1);
		count++;
	}
	/* Overtime labels: "OT", "OT2", "OT3", ... */
	if (g_state.overtime_enabled) {
		for (int i = 0; i < g_state.ot_max &&
				count < SCOREBOARD_MAX_PERIOD_LABELS;
		     i++) {
			if (i == 0)
				snprintf(g_state.period_labels[count],
					 SCOREBOARD_PERIOD_LABEL_SIZE, "OT");
			else
				snprintf(g_state.period_labels[count],
					 SCOREBOARD_PERIOD_LABEL_SIZE, "OT%d",
					 i + 1);
			count++;
		}
	}
	g_state.period_label_count = count;
}

void scoreboard_set_period_labels(const char *labels)
{
	if (labels == NULL)
		return;

	int count = 0;
	const char *p = labels;
	while (*p != '\0' && count < SCOREBOARD_MAX_PERIOD_LABELS) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		/* Skip empty lines */
		if (len > 0) {
			if (len >= SCOREBOARD_PERIOD_LABEL_SIZE)
				len = SCOREBOARD_PERIOD_LABEL_SIZE - 1;
			memcpy(g_state.period_labels[count], p, len);
			g_state.period_labels[count][len] = '\0';
			count++;
		}
		p = nl ? nl + 1 : p + len;
	}
	if (count > 0) {
		g_state.period_label_count = count;
		/* Clamp current period to new label count */
		if (g_state.period > count)
			g_state.period = count;
		mark_dirty();
	}
}

void scoreboard_get_period_labels(char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	size_t offset = 0;
	for (int i = 0; i < g_state.period_label_count; i++) {
		size_t label_len = strlen(g_state.period_labels[i]);
		/* Need room for label + newline + null */
		if (offset + label_len + 1 >= size)
			break;
		memcpy(buf + offset, g_state.period_labels[i], label_len);
		offset += label_len;
		buf[offset++] = '\n';
	}
	buf[offset] = '\0';
}

int scoreboard_get_period_label_count(void)
{
	return g_state.period_label_count;
}

const char *scoreboard_get_period_label(int index)
{
	if (index < 0 || index >= g_state.period_label_count)
		return "";
	return g_state.period_labels[index];
}

void scoreboard_set_default_penalty_duration(int seconds)
{
	if (seconds < 1)
		seconds = 1;
	g_state.default_penalty_duration = seconds;
}

int scoreboard_get_default_penalty_duration(void)
{
	return g_state.default_penalty_duration;
}

void scoreboard_set_default_major_penalty_duration(int seconds)
{
	if (seconds < 1)
		seconds = 1;
	g_state.default_major_penalty_duration = seconds;
}

int scoreboard_get_default_major_penalty_duration(void)
{
	return g_state.default_major_penalty_duration;
}

/* ---- team names ---- */

void scoreboard_set_home_name(const char *name)
{
	safe_copy(g_state.home_name, name, sizeof(g_state.home_name));
	mark_dirty();
}

const char *scoreboard_get_home_name(void)
{
	return g_state.home_name;
}

void scoreboard_set_away_name(const char *name)
{
	safe_copy(g_state.away_name, name, sizeof(g_state.away_name));
	mark_dirty();
}

const char *scoreboard_get_away_name(void)
{
	return g_state.away_name;
}

/* ---- score ---- */

int scoreboard_get_home_score(void)
{
	return g_state.home_score;
}

void scoreboard_set_home_score(int score)
{
	int new_score = score < 0 ? 0 : score;
	/* Only a real change breaks the link to recorded goals. */
	if (new_score != g_state.home_score)
		pm_forget_goals(true);
	g_state.home_score = new_score;
	mark_dirty();
}

void scoreboard_increment_home_score(void)
{
	g_state.home_score++;
	pm_record_goal(true);
	mark_dirty();
}

void scoreboard_decrement_home_score(void)
{
	if (g_state.home_score > 0) {
		g_state.home_score--;
		pm_undo_goal(true);
	}
	mark_dirty();
}

int scoreboard_get_away_score(void)
{
	return g_state.away_score;
}

void scoreboard_set_away_score(int score)
{
	int new_score = score < 0 ? 0 : score;
	/* Only a real change breaks the link to recorded goals. */
	if (new_score != g_state.away_score)
		pm_forget_goals(false);
	g_state.away_score = new_score;
	mark_dirty();
}

void scoreboard_increment_away_score(void)
{
	g_state.away_score++;
	pm_record_goal(false);
	release_home_minor_for_goal();
	mark_dirty();
}

void scoreboard_decrement_away_score(void)
{
	if (g_state.away_score > 0) {
		g_state.away_score--;
		pm_undo_goal(false);
	}
	mark_dirty();
}

/* ---- shots ---- */

int scoreboard_get_home_shots(void)
{
	return g_state.home_shots;
}

void scoreboard_set_home_shots(int shots)
{
	g_state.home_shots = shots < 0 ? 0 : shots;
	mark_dirty();
}

void scoreboard_increment_home_shots(void)
{
	g_state.home_shots++;
	mark_dirty();
}

void scoreboard_decrement_home_shots(void)
{
	if (g_state.home_shots > 0)
		g_state.home_shots--;
	mark_dirty();
}

int scoreboard_get_away_shots(void)
{
	return g_state.away_shots;
}

void scoreboard_set_away_shots(int shots)
{
	g_state.away_shots = shots < 0 ? 0 : shots;
	mark_dirty();
}

void scoreboard_increment_away_shots(void)
{
	g_state.away_shots++;
	goalie_add_shot(1);
	mark_dirty();
}

void scoreboard_decrement_away_shots(void)
{
	if (g_state.away_shots > 0) {
		g_state.away_shots--;
		goalie_add_shot(-1);
	}
	mark_dirty();
}

/* ---- faceoffs ---- */

int scoreboard_get_home_faceoffs(void)
{
	return g_state.home_faceoffs;
}

void scoreboard_set_home_faceoffs(int faceoffs)
{
	g_state.home_faceoffs = faceoffs < 0 ? 0 : faceoffs;
	mark_dirty();
}

void scoreboard_increment_home_faceoffs(void)
{
	g_state.home_faceoffs++;
	mark_dirty();
}

void scoreboard_decrement_home_faceoffs(void)
{
	if (g_state.home_faceoffs > 0)
		g_state.home_faceoffs--;
	mark_dirty();
}

int scoreboard_get_away_faceoffs(void)
{
	return g_state.away_faceoffs;
}

void scoreboard_set_away_faceoffs(int faceoffs)
{
	g_state.away_faceoffs = faceoffs < 0 ? 0 : faceoffs;
	mark_dirty();
}

void scoreboard_increment_away_faceoffs(void)
{
	g_state.away_faceoffs++;
	mark_dirty();
}

void scoreboard_decrement_away_faceoffs(void)
{
	if (g_state.away_faceoffs > 0)
		g_state.away_faceoffs--;
	mark_dirty();
}

bool scoreboard_get_has_faceoffs(void)
{
	return g_state.has_faceoffs;
}

/* ---- fouls ---- */

int scoreboard_get_home_fouls(void)
{
	return g_state.home_fouls;
}

void scoreboard_set_home_fouls(int fouls)
{
	g_state.home_fouls = fouls < 0 ? 0 : fouls;
	mark_dirty();
}

void scoreboard_increment_home_fouls(void)
{
	g_state.home_fouls++;
	mark_dirty();
}

void scoreboard_decrement_home_fouls(void)
{
	if (g_state.home_fouls > 0)
		g_state.home_fouls--;
	mark_dirty();
}

int scoreboard_get_away_fouls(void)
{
	return g_state.away_fouls;
}

void scoreboard_set_away_fouls(int fouls)
{
	g_state.away_fouls = fouls < 0 ? 0 : fouls;
	mark_dirty();
}

void scoreboard_increment_away_fouls(void)
{
	g_state.away_fouls++;
	mark_dirty();
}

void scoreboard_decrement_away_fouls(void)
{
	if (g_state.away_fouls > 0)
		g_state.away_fouls--;
	mark_dirty();
}

/* ---- fouls2 ---- */

int scoreboard_get_home_fouls2(void)
{
	return g_state.home_fouls2;
}

void scoreboard_set_home_fouls2(int fouls)
{
	g_state.home_fouls2 = fouls < 0 ? 0 : fouls;
	mark_dirty();
}

void scoreboard_increment_home_fouls2(void)
{
	g_state.home_fouls2++;
	mark_dirty();
}

void scoreboard_decrement_home_fouls2(void)
{
	if (g_state.home_fouls2 > 0)
		g_state.home_fouls2--;
	mark_dirty();
}

int scoreboard_get_away_fouls2(void)
{
	return g_state.away_fouls2;
}

void scoreboard_set_away_fouls2(int fouls)
{
	g_state.away_fouls2 = fouls < 0 ? 0 : fouls;
	mark_dirty();
}

void scoreboard_increment_away_fouls2(void)
{
	g_state.away_fouls2++;
	mark_dirty();
}

void scoreboard_decrement_away_fouls2(void)
{
	if (g_state.away_fouls2 > 0)
		g_state.away_fouls2--;
	mark_dirty();
}

/* ---- penalties ---- */

int scoreboard_home_penalty_add(int player_number, int duration_secs)
{
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!g_state.home_penalties[i].active) {
			g_state.home_penalties[i].player_number = player_number;
			g_state.home_penalties[i].remaining_tenths =
				duration_secs * 10;
			g_state.home_penalties[i].major =
				duration_secs >= SCOREBOARD_MAJOR_SECS;
			g_state.home_penalties[i].active = true;
			pim_add(player_number, duration_secs);
			mark_dirty();
			return i;
		}
	}
	return -1;
}

int scoreboard_home_penalty_add_compound(int player_number, int phase1_secs,
					 int phase2_secs)
{
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!g_state.home_penalties[i].active) {
			g_state.home_penalties[i].player_number = player_number;
			g_state.home_penalties[i].remaining_tenths =
				phase1_secs * 10;
			g_state.home_penalties[i].phase2_tenths =
				phase2_secs * 10;
			g_state.home_penalties[i].major =
				phase1_secs >= SCOREBOARD_MAJOR_SECS;
			g_state.home_penalties[i].active = true;
			pim_add(player_number, phase1_secs + phase2_secs);
			mark_dirty();
			return i;
		}
	}
	return -1;
}

void scoreboard_home_penalty_clear(int slot)
{
	if (slot >= 0 && slot < SCOREBOARD_PENALTY_SLOTS) {
		g_state.home_penalties[slot].active = false;
		g_state.home_penalties[slot].player_number = 0;
		g_state.home_penalties[slot].remaining_tenths = 0;
		g_state.home_penalties[slot].phase2_tenths = 0;
		g_state.home_penalties[slot].major = false;
		mark_dirty();
	}
}

void scoreboard_home_penalty_set_time(int slot, int duration_secs)
{
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return;
	if (!g_state.home_penalties[slot].active)
		return;
	if (duration_secs <= 0) {
		if (g_state.home_penalties[slot].phase2_tenths > 0) {
			g_state.home_penalties[slot].remaining_tenths =
				g_state.home_penalties[slot].phase2_tenths;
			penalty_phase_two(&g_state.home_penalties[slot]);
		} else {
			scoreboard_home_penalty_clear(slot);
			scoreboard_penalty_compact();
			return;
		}
	} else {
		g_state.home_penalties[slot].remaining_tenths =
			duration_secs * 10;
	}
	mark_dirty();
}

const struct scoreboard_penalty *scoreboard_get_home_penalty(int slot)
{
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return NULL;
	return &g_state.home_penalties[slot];
}

int scoreboard_away_penalty_add(int player_number, int duration_secs)
{
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!g_state.away_penalties[i].active) {
			g_state.away_penalties[i].player_number = player_number;
			g_state.away_penalties[i].remaining_tenths =
				duration_secs * 10;
			g_state.away_penalties[i].major =
				duration_secs >= SCOREBOARD_MAJOR_SECS;
			g_state.away_penalties[i].active = true;
			mark_dirty();
			return i;
		}
	}
	return -1;
}

int scoreboard_away_penalty_add_compound(int player_number, int phase1_secs,
					 int phase2_secs)
{
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!g_state.away_penalties[i].active) {
			g_state.away_penalties[i].player_number = player_number;
			g_state.away_penalties[i].remaining_tenths =
				phase1_secs * 10;
			g_state.away_penalties[i].phase2_tenths =
				phase2_secs * 10;
			g_state.away_penalties[i].major =
				phase1_secs >= SCOREBOARD_MAJOR_SECS;
			g_state.away_penalties[i].active = true;
			mark_dirty();
			return i;
		}
	}
	return -1;
}

void scoreboard_away_penalty_clear(int slot)
{
	if (slot >= 0 && slot < SCOREBOARD_PENALTY_SLOTS) {
		g_state.away_penalties[slot].active = false;
		g_state.away_penalties[slot].player_number = 0;
		g_state.away_penalties[slot].remaining_tenths = 0;
		g_state.away_penalties[slot].phase2_tenths = 0;
		g_state.away_penalties[slot].major = false;
		mark_dirty();
	}
}

void scoreboard_away_penalty_set_time(int slot, int duration_secs)
{
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return;
	if (!g_state.away_penalties[slot].active)
		return;
	if (duration_secs <= 0) {
		if (g_state.away_penalties[slot].phase2_tenths > 0) {
			g_state.away_penalties[slot].remaining_tenths =
				g_state.away_penalties[slot].phase2_tenths;
			penalty_phase_two(&g_state.away_penalties[slot]);
		} else {
			scoreboard_away_penalty_clear(slot);
			scoreboard_penalty_compact();
			return;
		}
	} else {
		g_state.away_penalties[slot].remaining_tenths =
			duration_secs * 10;
	}
	mark_dirty();
}

const struct scoreboard_penalty *scoreboard_get_away_penalty(int slot)
{
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return NULL;
	return &g_state.away_penalties[slot];
}

int scoreboard_get_home_penalty_count(void)
{
	int count = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (g_state.home_penalties[i].active)
			count++;
	}
	return count;
}

int scoreboard_get_away_penalty_count(void)
{
	int count = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (g_state.away_penalties[i].active)
			count++;
	}
	return count;
}

void scoreboard_penalty_tick(int elapsed_tenths)
{
	bool ticked = false;
	bool cleared = false;
	int home_running = 0;
	int away_running = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (g_state.home_penalties[i].active &&
		    home_running < SCOREBOARD_MAX_RUNNING_PENALTIES) {
			g_state.home_penalties[i].remaining_tenths -=
				elapsed_tenths;
			home_running++;
			ticked = true;
			if (g_state.home_penalties[i].remaining_tenths <= 0) {
				if (g_state.home_penalties[i].phase2_tenths >
				    0) {
					g_state.home_penalties[i]
						.remaining_tenths =
						g_state.home_penalties[i]
							.phase2_tenths;
					penalty_phase_two(&g_state.home_penalties[i]);
				} else {
					scoreboard_home_penalty_clear(i);
					cleared = true;
				}
			}
		}
		if (g_state.away_penalties[i].active &&
		    away_running < SCOREBOARD_MAX_RUNNING_PENALTIES) {
			g_state.away_penalties[i].remaining_tenths -=
				elapsed_tenths;
			away_running++;
			ticked = true;
			if (g_state.away_penalties[i].remaining_tenths <= 0) {
				if (g_state.away_penalties[i].phase2_tenths >
				    0) {
					g_state.away_penalties[i]
						.remaining_tenths =
						g_state.away_penalties[i]
							.phase2_tenths;
					penalty_phase_two(&g_state.away_penalties[i]);
				} else {
					scoreboard_away_penalty_clear(i);
					cleared = true;
				}
			}
		}
	}
	if (cleared)
		scoreboard_penalty_compact();
	if (ticked)
		mark_dirty();
}

void scoreboard_penalty_adjust(int delta_tenths)
{
	bool adjusted = false;
	bool cleared = false;
	int home_running = 0;
	int away_running = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (g_state.home_penalties[i].active &&
		    home_running < SCOREBOARD_MAX_RUNNING_PENALTIES) {
			g_state.home_penalties[i].remaining_tenths +=
				delta_tenths;
			home_running++;
			adjusted = true;
			if (g_state.home_penalties[i].remaining_tenths <= 0) {
				if (g_state.home_penalties[i].phase2_tenths >
				    0) {
					int leftover =
						g_state.home_penalties[i]
							.remaining_tenths;
					g_state.home_penalties[i]
						.remaining_tenths =
						g_state.home_penalties[i]
							.phase2_tenths +
						leftover;
					penalty_phase_two(&g_state.home_penalties[i]);
					if (g_state.home_penalties[i]
						    .remaining_tenths <= 0) {
						scoreboard_home_penalty_clear(
							i);
						cleared = true;
					}
				} else {
					scoreboard_home_penalty_clear(i);
					cleared = true;
				}
			}
		}
		if (g_state.away_penalties[i].active &&
		    away_running < SCOREBOARD_MAX_RUNNING_PENALTIES) {
			g_state.away_penalties[i].remaining_tenths +=
				delta_tenths;
			away_running++;
			adjusted = true;
			if (g_state.away_penalties[i].remaining_tenths <= 0) {
				if (g_state.away_penalties[i].phase2_tenths >
				    0) {
					int leftover =
						g_state.away_penalties[i]
							.remaining_tenths;
					g_state.away_penalties[i]
						.remaining_tenths =
						g_state.away_penalties[i]
							.phase2_tenths +
						leftover;
					penalty_phase_two(&g_state.away_penalties[i]);
					if (g_state.away_penalties[i]
						    .remaining_tenths <= 0) {
						scoreboard_away_penalty_clear(
							i);
						cleared = true;
					}
				} else {
					scoreboard_away_penalty_clear(i);
					cleared = true;
				}
			}
		}
	}
	if (cleared)
		scoreboard_penalty_compact();
	if (adjusted)
		mark_dirty();
}

static void compact_penalties(struct scoreboard_penalty *penalties)
{
	int w = 0;
	for (int r = 0; r < SCOREBOARD_PENALTY_SLOTS; r++) {
		if (penalties[r].active) {
			if (w != r)
				penalties[w] = penalties[r];
			w++;
		}
	}
	for (int i = w; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		penalties[i].active = false;
		penalties[i].player_number = 0;
		penalties[i].remaining_tenths = 0;
		penalties[i].phase2_tenths = 0;
		penalties[i].major = false;
	}
}

void scoreboard_penalty_compact(void)
{
	compact_penalties(g_state.home_penalties);
	compact_penalties(g_state.away_penalties);
}

void scoreboard_format_penalty_number(int slot, bool home, char *buf,
				      size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return;
	const struct scoreboard_penalty *p =
		home ? &g_state.home_penalties[slot]
		     : &g_state.away_penalties[slot];
	if (!p->active)
		return;
	if (p->player_number > 0)
		snprintf(buf, size, "#%d", p->player_number);
	else
		snprintf(buf, size, " ");
}

void scoreboard_format_penalty_time(int slot, bool home, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	if (slot < 0 || slot >= SCOREBOARD_PENALTY_SLOTS)
		return;
	const struct scoreboard_penalty *p =
		home ? &g_state.home_penalties[slot]
		     : &g_state.away_penalties[slot];
	if (!p->active)
		return;
	int total_seconds = p->remaining_tenths / 10;
	int minutes = total_seconds / 60;
	int seconds = total_seconds % 60;
	snprintf(buf, size, "%d:%02d", minutes, seconds);
}

void scoreboard_format_all_penalty_numbers(bool home, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	const struct scoreboard_penalty *penalties =
		home ? g_state.home_penalties : g_state.away_penalties;
	size_t offset = 0;
	int running = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!penalties[i].active)
			continue;
		if (running >= SCOREBOARD_MAX_RUNNING_PENALTIES)
			break;
		running++;
		char line[32];
		if (penalties[i].player_number > 0)
			snprintf(line, sizeof(line), "#%d",
				 penalties[i].player_number);
		else
			snprintf(line, sizeof(line), " ");
		size_t len = strlen(line);
		size_t need = (offset > 0 ? 1 : 0) + len;
		if (offset + need >= size)
			break;
		if (offset > 0)
			buf[offset++] = '\n';
		memcpy(buf + offset, line, len);
		offset += len;
	}
	buf[offset] = '\0';
}

void scoreboard_format_all_penalty_times(bool home, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	const struct scoreboard_penalty *penalties =
		home ? g_state.home_penalties : g_state.away_penalties;
	size_t offset = 0;
	int running = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!penalties[i].active)
			continue;
		if (running >= SCOREBOARD_MAX_RUNNING_PENALTIES)
			break;
		running++;
		int total_seconds = penalties[i].remaining_tenths / 10;
		int minutes = total_seconds / 60;
		int seconds = total_seconds % 60;
		char line[32];
		snprintf(line, sizeof(line), "%d:%02d", minutes, seconds);
		size_t len = strlen(line);
		size_t need = (offset > 0 ? 1 : 0) + len;
		if (offset + need >= size)
			break;
		if (offset > 0)
			buf[offset++] = '\n';
		memcpy(buf + offset, line, len);
		offset += len;
	}
	buf[offset] = '\0';
}

void scoreboard_set_penalty_label_format(const char *fmt)
{
	safe_copy(g_state.penalty_label_format, fmt,
		  sizeof(g_state.penalty_label_format));
	mark_dirty();
}

const char *scoreboard_get_penalty_label_format(void)
{
	if (g_state.penalty_label_format[0] == '\0')
		return kDefaultPenaltyLabelFormat;
	return g_state.penalty_label_format;
}

/* Expand a penalty label format string for a single penalty.
   Replaces {{ number }} with the player number and {{ time }} with
   the remaining time.  Unknown variables become empty strings. */
static size_t expand_penalty_format(const char *fmt, const char *number,
				    const char *time_str,
				    const char *phase2_str, char *buf,
				    size_t size)
{
	size_t out = 0;
	const char *p = fmt;
	while (*p != '\0' && out < size - 1) {
		if (p[0] == '{' && p[1] == '{') {
			const char *end = strstr(p + 2, "}}");
			if (end == NULL) {
				/* Unterminated — copy rest literally */
				break;
			}
			/* Extract variable name, trim whitespace */
			const char *vs = p + 2;
			while (vs < end && *vs == ' ')
				vs++;
			const char *ve = end;
			while (ve > vs && *(ve - 1) == ' ')
				ve--;
			size_t nlen = (size_t)(ve - vs);
			if (nlen == 9 &&
			    strncmp(vs, "if_phase2", 9) == 0) {
				/* Conditional block: skip to end_if when
				   phase2 is empty */
				p = end + 2;
				if (phase2_str[0] == '\0') {
					const char *skip = p;
					while (*skip != '\0') {
						if (skip[0] == '{' &&
						    skip[1] == '{') {
							const char *se =
								strstr(skip +
									       2,
								       "}}");
							if (se != NULL) {
								const char *ts =
									skip +
									2;
								while (ts <
									       se &&
								       *ts ==
									       ' ')
									ts++;
								const char *te =
									se;
								while (te >
									       ts &&
								       *(te - 1) ==
									       ' ')
									te--;
								if ((size_t)(
									    te -
									    ts) ==
									    6 &&
								    strncmp(ts,
									    "end_if",
									    6) ==
									    0) {
									p = se +
									    2;
									break;
								}
							}
						}
						skip++;
					}
					if (*skip == '\0')
						p = skip;
				}
				continue;
			}
			if (nlen == 6 &&
			    strncmp(vs, "end_if", 6) == 0) {
				/* End of conditional block — just skip */
				p = end + 2;
				continue;
			}
			const char *val = "";
			if (nlen == 6 && strncmp(vs, "number", 6) == 0)
				val = number;
			else if (nlen == 4 && strncmp(vs, "time", 4) == 0)
				val = time_str;
			else if (nlen == 6 && strncmp(vs, "phase2", 6) == 0)
				val = phase2_str;
			size_t vlen = strlen(val);
			memcpy(buf + out, val, vlen);
			out += vlen;
			p = end + 2;
		} else {
			buf[out++] = *p++;
		}
	}
	/* Copy any remaining literal text after unterminated {{ */
	while (*p != '\0' && out < size - 1)
		buf[out++] = *p++;
	buf[out] = '\0';
	return out;
}

void scoreboard_format_penalty_labels(bool home, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	buf[0] = '\0';
	const struct scoreboard_penalty *penalties =
		home ? g_state.home_penalties : g_state.away_penalties;
	const char *fmt = scoreboard_get_penalty_label_format();
	size_t offset = 0;
	int running = 0;
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		if (!penalties[i].active)
			continue;
		if (running >= SCOREBOARD_MAX_RUNNING_PENALTIES)
			break;
		running++;

		/* Format number */
		char num_buf[32];
		if (penalties[i].player_number > 0)
			snprintf(num_buf, sizeof(num_buf), "%d",
				 penalties[i].player_number);
		else
			num_buf[0] = '\0';

		/* Format time */
		char time_buf[32];
		int total_seconds = penalties[i].remaining_tenths / 10;
		int minutes = total_seconds / 60;
		int seconds = total_seconds % 60;
		snprintf(time_buf, sizeof(time_buf), "%d:%02d", minutes,
			 seconds);

		/* Format phase2 (compound penalties only) */
		char phase2_buf[32];
		if (penalties[i].phase2_tenths > 0) {
			int p2_secs = penalties[i].phase2_tenths / 10;
			int p2_min = p2_secs / 60;
			int p2_sec = p2_secs % 60;
			snprintf(phase2_buf, sizeof(phase2_buf), "%d:%02d",
				 p2_min, p2_sec);
		} else {
			phase2_buf[0] = '\0';
		}

		/* Expand format for this penalty */
		char line[SCOREBOARD_PENALTY_LABEL_FORMAT_SIZE];
		expand_penalty_format(fmt, num_buf, time_buf, phase2_buf,
				      line, sizeof(line));
		size_t len = strlen(line);
		size_t need = (offset > 0 ? 1 : 0) + len;
		if (offset + need >= size)
			break;
		if (offset > 0)
			buf[offset++] = '\n';
		memcpy(buf + offset, line, len);
		offset += len;
	}
	buf[offset] = '\0';
}

void scoreboard_preview_penalty_label(const char *fmt, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	if (fmt == NULL || fmt[0] == '\0')
		fmt = kDefaultPenaltyLabelFormat;

	/* Sample regular penalty: #23, 1:30 remaining */
	char line1[SCOREBOARD_PENALTY_LABEL_FORMAT_SIZE];
	expand_penalty_format(fmt, "23", "1:30", "", line1, sizeof(line1));

	/* Sample compound penalty: #88, 0:45 phase1, 5:00 phase2 */
	char line2[SCOREBOARD_PENALTY_LABEL_FORMAT_SIZE];
	expand_penalty_format(fmt, "88", "0:45", "5:00", line2,
			      sizeof(line2));

	snprintf(buf, size, "%s\n%s", line1, line2);
}

/* ---- file output ---- */

void scoreboard_set_output_directory(const char *path)
{
	safe_copy(g_state.output_directory, path,
		  sizeof(g_state.output_directory));
}

const char *scoreboard_get_output_directory(void)
{
	return g_state.output_directory;
}

bool scoreboard_write_all_files(void)
{
	if (!g_dirty)
		return true;

	const char *dir = g_state.output_directory;
	if (dir[0] == '\0')
		return false;

	char buf[64];
	bool ok = true;

	scoreboard_clock_format(buf, sizeof(buf));
	ok = write_text_file(dir, "clock.txt", buf) && ok;

	scoreboard_format_period(buf, sizeof(buf));
	ok = write_text_file(dir, "period.txt", buf) && ok;

	ok = write_text_file(dir, "home_name.txt", g_state.home_name) && ok;
	ok = write_text_file(dir, "away_name.txt", g_state.away_name) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.home_score);
	ok = write_text_file(dir, "home_score.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.away_score);
	ok = write_text_file(dir, "away_score.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.home_shots);
	ok = write_text_file(dir, "home_shots.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.away_shots);
	ok = write_text_file(dir, "away_shots.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.home_faceoffs);
	ok = write_text_file(dir, "home_faceoffs.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.away_faceoffs);
	ok = write_text_file(dir, "away_faceoffs.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.home_fouls);
	ok = write_text_file(dir, "home_fouls.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.away_fouls);
	ok = write_text_file(dir, "away_fouls.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.home_fouls2);
	ok = write_text_file(dir, "home_fouls2.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d", g_state.away_fouls2);
	ok = write_text_file(dir, "away_fouls2.txt", buf) && ok;

	char pen_buf[512];

	scoreboard_format_all_penalty_numbers(true, pen_buf, sizeof(pen_buf));
	ok = write_text_file(dir, "home_penalty_numbers.txt", pen_buf) && ok;

	scoreboard_format_all_penalty_times(true, pen_buf, sizeof(pen_buf));
	ok = write_text_file(dir, "home_penalty_times.txt", pen_buf) && ok;

	scoreboard_format_all_penalty_numbers(false, pen_buf, sizeof(pen_buf));
	ok = write_text_file(dir, "away_penalty_numbers.txt", pen_buf) && ok;

	scoreboard_format_all_penalty_times(false, pen_buf, sizeof(pen_buf));
	ok = write_text_file(dir, "away_penalty_times.txt", pen_buf) && ok;

	{
		char labels_buf[512];
		scoreboard_format_penalty_labels(true, labels_buf,
						 sizeof(labels_buf));
		ok = write_text_file(dir, "home_penalty_labels.txt",
				     labels_buf) &&
		     ok;
		scoreboard_format_penalty_labels(false, labels_buf,
						 sizeof(labels_buf));
		ok = write_text_file(dir, "away_penalty_labels.txt",
				     labels_buf) &&
		     ok;
	}

	ok = write_text_file(dir, "sport.txt",
			     scoreboard_sport_name(g_state.sport)) &&
	     ok;

	snprintf(buf, sizeof(buf), "%d",
		 g_state.default_penalty_duration);
	ok = write_text_file(dir, "default_penalty_duration.txt", buf) && ok;

	snprintf(buf, sizeof(buf), "%d",
		 g_state.default_major_penalty_duration);
	ok = write_text_file(dir, "default_major_penalty_duration.txt", buf) &&
	     ok;

	{
		char labels_buf[512];
		scoreboard_get_period_labels(labels_buf, sizeof(labels_buf));
		ok = write_text_file(dir, "period_labels.txt", labels_buf) &&
		     ok;
	}

	snprintf(buf, sizeof(buf), "%d", g_state.period_length);
	ok = write_text_file(dir, "period_length.txt", buf) && ok;

	if (g_state.game_clock_enabled) {
		char gc_buf[32];
		scoreboard_game_clock_format(gc_buf, sizeof(gc_buf));
		ok = write_text_file(dir, "cumulative_clock.txt", gc_buf) && ok;
	}

	if (g_state.base_strength > 0) {
		char strength_buf[SCOREBOARD_STRENGTH_LABEL_FORMAT_SIZE];
		scoreboard_format_strength(strength_buf,
					   sizeof(strength_buf));
		ok = write_text_file(dir, "strength.txt", strength_buf) && ok;
	}

	{
		char pm_buf[2048];
		scoreboard_format_plus_minus_lines(false, pm_buf,
						   sizeof(pm_buf));
		ok = write_text_file(dir, "home_plus_minus.txt", pm_buf) && ok;
		scoreboard_format_plus_minus_lines(true, pm_buf,
						   sizeof(pm_buf));
		ok = write_text_file(dir, "home_season_plus_minus.txt",
				     pm_buf) && ok;
		scoreboard_format_scoring_lines(false, pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_scoring.txt", pm_buf) && ok;
		scoreboard_format_scoring_lines(true, pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_season_scoring.txt", pm_buf) &&
		     ok;
		scoreboard_format_last_goal(pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "last_goal.txt", pm_buf) && ok;
		scoreboard_format_pim_lines(pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_pim.txt", pm_buf) && ok;
		scoreboard_format_ppg_lines(pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_ppg.txt", pm_buf) && ok;
		scoreboard_format_home_faceoff_percent(pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_faceoff_percent.txt", pm_buf) &&
		     ok;
		scoreboard_format_goalie_in_net(pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_goalie.txt", pm_buf) && ok;
		scoreboard_format_goalie_lines(false, pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_goalies.txt", pm_buf) && ok;
		scoreboard_format_goalie_lines(true, pm_buf, sizeof(pm_buf));
		ok = write_text_file(dir, "home_goalies_season.txt", pm_buf) &&
		     ok;
	}

	g_dirty = false;
	return ok;
}

bool scoreboard_read_all_files(void)
{
	const char *dir = g_state.output_directory;
	if (dir[0] == '\0')
		return false;

	char buf[512];
	bool ok = true;

	if (read_text_file(dir, "clock.txt", buf, sizeof(buf))) {
		int tenths = parse_clock_text(buf);
		if (tenths >= 0)
			g_state.clock_tenths = tenths;
	} else {
		ok = false;
	}

	if (read_text_file(dir, "period.txt", buf, sizeof(buf))) {
		int p = parse_period_text(buf);
		if (p > 0)
			scoreboard_set_period(p);
	} else {
		ok = false;
	}

	if (read_text_file(dir, "home_name.txt", buf, sizeof(buf)))
		scoreboard_set_home_name(buf);
	else
		ok = false;

	if (read_text_file(dir, "away_name.txt", buf, sizeof(buf)))
		scoreboard_set_away_name(buf);
	else
		ok = false;

	if (read_text_file(dir, "home_score.txt", buf, sizeof(buf)))
		scoreboard_set_home_score(atoi(buf));
	else
		ok = false;

	if (read_text_file(dir, "away_score.txt", buf, sizeof(buf)))
		scoreboard_set_away_score(atoi(buf));
	else
		ok = false;

	if (read_text_file(dir, "home_shots.txt", buf, sizeof(buf)))
		scoreboard_set_home_shots(atoi(buf));
	else
		ok = false;

	if (read_text_file(dir, "away_shots.txt", buf, sizeof(buf)))
		scoreboard_set_away_shots(atoi(buf));
	else
		ok = false;

	/* Faceoff files are optional — missing doesn't fail the read */
	if (read_text_file(dir, "home_faceoffs.txt", buf, sizeof(buf)))
		scoreboard_set_home_faceoffs(atoi(buf));
	if (read_text_file(dir, "away_faceoffs.txt", buf, sizeof(buf)))
		scoreboard_set_away_faceoffs(atoi(buf));

	/* Fouls files are optional — missing doesn't fail the read */
	if (read_text_file(dir, "home_fouls.txt", buf, sizeof(buf)))
		scoreboard_set_home_fouls(atoi(buf));
	if (read_text_file(dir, "away_fouls.txt", buf, sizeof(buf)))
		scoreboard_set_away_fouls(atoi(buf));
	if (read_text_file(dir, "home_fouls2.txt", buf, sizeof(buf)))
		scoreboard_set_home_fouls2(atoi(buf));
	if (read_text_file(dir, "away_fouls2.txt", buf, sizeof(buf)))
		scoreboard_set_away_fouls2(atoi(buf));

	char nums_buf[512], times_buf[512];
	bool hn = read_text_file(dir, "home_penalty_numbers.txt", nums_buf,
				 sizeof(nums_buf));
	bool ht = read_text_file(dir, "home_penalty_times.txt", times_buf,
				 sizeof(times_buf));
	if (hn && ht)
		parse_penalty_files(nums_buf, times_buf, true);
	else if (!hn || !ht)
		ok = false;

	bool an = read_text_file(dir, "away_penalty_numbers.txt", nums_buf,
				 sizeof(nums_buf));
	bool at = read_text_file(dir, "away_penalty_times.txt", times_buf,
				 sizeof(times_buf));
	if (an && at)
		parse_penalty_files(nums_buf, times_buf, false);
	else if (!an || !at)
		ok = false;

	/* Sport file is optional — missing doesn't fail the read */
	if (read_text_file(dir, "sport.txt", buf, sizeof(buf))) {
		enum scoreboard_sport s = scoreboard_sport_from_name(buf);
		if (s != g_state.sport)
			scoreboard_set_sport(s);
	}

	/* Penalty duration files are optional */
	if (read_text_file(dir, "default_penalty_duration.txt", buf,
			   sizeof(buf))) {
		int val = atoi(buf);
		if (val > 0)
			g_state.default_penalty_duration = val;
	}
	if (read_text_file(dir, "default_major_penalty_duration.txt", buf,
			   sizeof(buf))) {
		int val = atoi(buf);
		if (val > 0)
			g_state.default_major_penalty_duration = val;
	}

	/* Period labels file is optional — overrides sport defaults */
	if (read_text_file(dir, "period_labels.txt", buf, sizeof(buf)))
		scoreboard_set_period_labels(buf);

	/* Period length file is optional — overrides sport-set period_length */
	if (read_text_file(dir, "period_length.txt", buf, sizeof(buf))) {
		int val = atoi(buf);
		if (val > 0)
			g_state.period_length = val;
	}

	/* Cumulative clock file is optional — restores game clock state.
	   Must come after clock.txt and period_length.txt since we need
	   those to compute current-period elapsed and subtract it. */
	if (g_state.game_clock_enabled &&
	    read_text_file(dir, "cumulative_clock.txt", buf, sizeof(buf))) {
		int total_tenths = parse_cumulative_clock_text(buf);
		if (total_tenths >= 0) {
			int current_elapsed = current_period_elapsed_tenths();
			int accumulated = total_tenths - current_elapsed;
			if (accumulated < 0)
				accumulated = 0;
			g_state.game_clock_accumulated_tenths = accumulated;
			g_state.game_clock_started = true;
		}
	}

	g_dirty = false;
	return ok;
}

/* ---- state persistence ---- */

bool scoreboard_save_state(const char *path)
{
	if (path == NULL)
		return false;
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return false;

	fprintf(f, "{\n");
	fprintf(f, "  \"clock_tenths\": %d,\n", g_state.clock_tenths);
	fprintf(f, "  \"clock_running\": %s,\n",
		g_state.clock_running ? "true" : "false");
	fprintf(f, "  \"clock_direction\": %d,\n",
		(int)g_state.clock_direction);
	fprintf(f, "  \"period_length\": %d,\n", g_state.period_length);
	fprintf(f, "  \"period\": %d,\n", g_state.period);
	fprintf(f, "  \"overtime_enabled\": %s,\n",
		g_state.overtime_enabled ? "true" : "false");
	fprintf(f, "  \"game_clock_enabled\": %s,\n",
		g_state.game_clock_enabled ? "true" : "false");
	fprintf(f, "  \"game_clock_accumulated_tenths\": %d,\n",
		g_state.game_clock_accumulated_tenths);
	fprintf(f, "  \"game_clock_started\": %s,\n",
		g_state.game_clock_started ? "true" : "false");
	fprintf(f, "  \"game_clock_display_format\": %d,\n",
		(int)g_state.game_clock_display_format);
	write_json_string(f, "penalty_label_format",
			  g_state.penalty_label_format, false);
	write_json_string(f, "home_name", g_state.home_name, false);
	write_json_string(f, "away_name", g_state.away_name, false);
	fprintf(f, "  \"home_score\": %d,\n", g_state.home_score);
	fprintf(f, "  \"away_score\": %d,\n", g_state.away_score);
	fprintf(f, "  \"home_shots\": %d,\n", g_state.home_shots);
	fprintf(f, "  \"away_shots\": %d,\n", g_state.away_shots);
	fprintf(f, "  \"home_faceoffs\": %d,\n", g_state.home_faceoffs);
	fprintf(f, "  \"away_faceoffs\": %d,\n", g_state.away_faceoffs);
	fprintf(f, "  \"home_fouls\": %d,\n", g_state.home_fouls);
	fprintf(f, "  \"away_fouls\": %d,\n", g_state.away_fouls);
	fprintf(f, "  \"home_fouls2\": %d,\n", g_state.home_fouls2);
	fprintf(f, "  \"away_fouls2\": %d,\n", g_state.away_fouls2);
	write_json_string(f, "sport", scoreboard_sport_name(g_state.sport),
			  false);
	fprintf(f, "  \"base_strength\": %d,\n", g_state.base_strength);
	write_json_string(f, "strength_label_format",
			  g_state.strength_label_format, false);

	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		fprintf(f, "  \"home_penalty%d_number\": %d,\n", i,
			g_state.home_penalties[i].player_number);
		fprintf(f, "  \"home_penalty%d_tenths\": %d,\n", i,
			g_state.home_penalties[i].remaining_tenths);
		fprintf(f, "  \"home_penalty%d_active\": %s,\n", i,
			g_state.home_penalties[i].active ? "true" : "false");
		fprintf(f, "  \"home_penalty%d_phase2_tenths\": %d,\n", i,
			g_state.home_penalties[i].phase2_tenths);
		fprintf(f, "  \"home_penalty%d_major\": %s,\n", i,
			g_state.home_penalties[i].major ? "true" : "false");
	}
	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		fprintf(f, "  \"away_penalty%d_number\": %d,\n", i,
			g_state.away_penalties[i].player_number);
		fprintf(f, "  \"away_penalty%d_tenths\": %d,\n", i,
			g_state.away_penalties[i].remaining_tenths);
		fprintf(f, "  \"away_penalty%d_active\": %s,\n", i,
			g_state.away_penalties[i].active ? "true" : "false");
		fprintf(f, "  \"away_penalty%d_phase2_tenths\": %d,\n", i,
			g_state.away_penalties[i].phase2_tenths);
		fprintf(f, "  \"away_penalty%d_major\": %s,\n", i,
			g_state.away_penalties[i].major ? "true" : "false");
	}

	fprintf(f, "  \"pm_skip_power_play\": %s,\n",
		g_state.pm_skip_power_play ? "true" : "false");
	fprintf(f, "  \"home_roster_count\": %d,\n", g_state.home_roster_count);
	for (int i = 0; i < g_state.home_roster_count; i++) {
		const struct scoreboard_player *p = &g_state.home_roster[i];
		fprintf(f, "  \"home_player%d_number\": %d,\n", i, p->number);
		fprintf(f, "  \"home_player%d_plus_minus\": %d,\n", i,
			p->plus_minus);
		fprintf(f, "  \"home_player%d_goals\": %d,\n", i, p->goals);
		fprintf(f, "  \"home_player%d_assists\": %d,\n", i, p->assists);
		fprintf(f, "  \"home_player%d_season_plus_minus\": %d,\n", i,
			p->season_plus_minus);
		fprintf(f, "  \"home_player%d_season_goals\": %d,\n", i,
			p->season_goals);
		fprintf(f, "  \"home_player%d_season_assists\": %d,\n", i,
			p->season_assists);
		fprintf(f, "  \"home_player%d_pim\": %d,\n", i, p->pim);
		fprintf(f, "  \"home_player%d_season_pim\": %d,\n", i,
			p->season_pim);
		fprintf(f, "  \"home_player%d_games\": %d,\n", i, p->games);
	}
	fprintf(f, "  \"goalie_count\": %d,\n", g_state.goalie_count);
	for (int i = 0; i < g_state.goalie_count; i++) {
		const struct scoreboard_goalie *g = &g_state.goalies[i];
		fprintf(f, "  \"goalie%d_number\": %d,\n", i, g->number);
		fprintf(f, "  \"goalie%d_sa\": %d,\n", i, g->shots_against);
		fprintf(f, "  \"goalie%d_ga\": %d,\n", i, g->goals_against);
		fprintf(f, "  \"goalie%d_season_sa\": %d,\n", i,
			g->season_shots_against);
		fprintf(f, "  \"goalie%d_season_ga\": %d,\n", i,
			g->season_goals_against);
		fprintf(f, "  \"goalie%d_games\": %d,\n", i, g->games);
		fprintf(f, "  \"goalie%d_played\": %s,\n", i,
			g->played ? "true" : "false");
	}
	fprintf(f, "  \"goalie_in_net\": %d,\n",
		scoreboard_get_goalie_in_net());
	fprintf(f, "  \"away_goal_ends_penalty\": %s,\n",
		g_state.away_goal_ends_penalty ? "true" : "false");

	fprintf(f, "  \"period_label_count\": %d",
		g_state.period_label_count);
	for (int i = 0; i < g_state.period_label_count; i++) {
		char key[32];
		snprintf(key, sizeof(key), "period_label%d", i);
		fprintf(f, ",\n");
		bool last = (i == g_state.period_label_count - 1);
		write_json_string(f, key, g_state.period_labels[i], last);
	}

	fprintf(f, "}\n");
	fclose(f);
	return true;
}

bool scoreboard_load_state(const char *path)
{
	if (path == NULL)
		return false;
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return false;

	fseek(f, 0, SEEK_END);
	long file_size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (file_size <= 0 || file_size > 65536) {
		fclose(f);
		return false;
	}

	char *json = (char *)malloc((size_t)file_size + 1);
	size_t read_size = fread(json, 1, (size_t)file_size, f);
	fclose(f);
	json[read_size] = '\0';

	/* Load sport first — set_sport() applies preset defaults for
	   direction, period_length, etc., which explicit fields override. */
	{
		char sport_str[32];
		parse_json_string(json, "sport", sport_str,
				  sizeof(sport_str));
		if (sport_str[0] != '\0')
			scoreboard_set_sport(
				scoreboard_sport_from_name(sport_str));
	}

	g_state.clock_tenths =
		parse_json_int(json, "clock_tenths", g_state.clock_tenths);
	g_state.clock_running =
		parse_json_bool(json, "clock_running", g_state.clock_running);
	g_state.clock_direction = (enum scoreboard_clock_direction)parse_json_int(
		json, "clock_direction", (int)g_state.clock_direction);
	g_state.period_length =
		parse_json_int(json, "period_length", g_state.period_length);
	g_state.period = parse_json_int(json, "period", g_state.period);
	g_state.overtime_enabled = parse_json_bool(json, "overtime_enabled",
						   g_state.overtime_enabled);
	g_state.game_clock_enabled = parse_json_bool(
		json, "game_clock_enabled", g_state.game_clock_enabled);
	g_state.game_clock_accumulated_tenths = parse_json_int(
		json, "game_clock_accumulated_tenths",
		g_state.game_clock_accumulated_tenths);
	g_state.game_clock_started = parse_json_bool(
		json, "game_clock_started", g_state.game_clock_started);
	g_state.game_clock_display_format =
		(enum scoreboard_game_clock_format)parse_json_int(
			json, "game_clock_display_format",
			(int)g_state.game_clock_display_format);

	parse_json_string(json, "penalty_label_format",
			  g_state.penalty_label_format,
			  sizeof(g_state.penalty_label_format));

	parse_json_string(json, "home_name", g_state.home_name,
			  sizeof(g_state.home_name));
	parse_json_string(json, "away_name", g_state.away_name,
			  sizeof(g_state.away_name));

	g_state.home_score =
		parse_json_int(json, "home_score", g_state.home_score);
	g_state.away_score =
		parse_json_int(json, "away_score", g_state.away_score);
	g_state.home_shots =
		parse_json_int(json, "home_shots", g_state.home_shots);
	g_state.away_shots =
		parse_json_int(json, "away_shots", g_state.away_shots);
	g_state.home_faceoffs =
		parse_json_int(json, "home_faceoffs", g_state.home_faceoffs);
	g_state.away_faceoffs =
		parse_json_int(json, "away_faceoffs", g_state.away_faceoffs);
	g_state.home_fouls =
		parse_json_int(json, "home_fouls", g_state.home_fouls);
	g_state.away_fouls =
		parse_json_int(json, "away_fouls", g_state.away_fouls);
	g_state.home_fouls2 =
		parse_json_int(json, "home_fouls2", g_state.home_fouls2);
	g_state.away_fouls2 =
		parse_json_int(json, "away_fouls2", g_state.away_fouls2);
	g_state.base_strength =
		parse_json_int(json, "base_strength", g_state.base_strength);
	parse_json_string(json, "strength_label_format",
			  g_state.strength_label_format,
			  sizeof(g_state.strength_label_format));

	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		char key[64];
		snprintf(key, sizeof(key), "home_penalty%d_number", i);
		g_state.home_penalties[i].player_number = parse_json_int(
			json, key, g_state.home_penalties[i].player_number);
		snprintf(key, sizeof(key), "home_penalty%d_tenths", i);
		g_state.home_penalties[i].remaining_tenths = parse_json_int(
			json, key, g_state.home_penalties[i].remaining_tenths);
		snprintf(key, sizeof(key), "home_penalty%d_active", i);
		g_state.home_penalties[i].active = parse_json_bool(
			json, key, g_state.home_penalties[i].active);
		snprintf(key, sizeof(key), "home_penalty%d_phase2_tenths", i);
		g_state.home_penalties[i].phase2_tenths = parse_json_int(
			json, key, g_state.home_penalties[i].phase2_tenths);
		snprintf(key, sizeof(key), "home_penalty%d_major", i);
		g_state.home_penalties[i].major = parse_json_bool(
			json, key, g_state.home_penalties[i].major);

		snprintf(key, sizeof(key), "away_penalty%d_number", i);
		g_state.away_penalties[i].player_number = parse_json_int(
			json, key, g_state.away_penalties[i].player_number);
		snprintf(key, sizeof(key), "away_penalty%d_tenths", i);
		g_state.away_penalties[i].remaining_tenths = parse_json_int(
			json, key, g_state.away_penalties[i].remaining_tenths);
		snprintf(key, sizeof(key), "away_penalty%d_active", i);
		g_state.away_penalties[i].active = parse_json_bool(
			json, key, g_state.away_penalties[i].active);
		snprintf(key, sizeof(key), "away_penalty%d_phase2_tenths", i);
		g_state.away_penalties[i].phase2_tenths = parse_json_int(
			json, key, g_state.away_penalties[i].phase2_tenths);
		snprintf(key, sizeof(key), "away_penalty%d_major", i);
		g_state.away_penalties[i].major = parse_json_bool(
			json, key, g_state.away_penalties[i].major);
	}

	g_state.pm_skip_power_play = parse_json_bool(
		json, "pm_skip_power_play", g_state.pm_skip_power_play);
	{
		/* An older state file may have no roster; keep the current one. */
		int rcount = parse_json_int(json, "home_roster_count", -1);
		if (rcount > SCOREBOARD_MAX_ROSTER)
			rcount = SCOREBOARD_MAX_ROSTER;
		for (int i = 0; i < rcount; i++) {
			struct scoreboard_player *p = &g_state.home_roster[i];
			char key[64];
			snprintf(key, sizeof(key), "home_player%d_number", i);
			p->number = parse_json_int(json, key, 0);
			snprintf(key, sizeof(key), "home_player%d_plus_minus",
				 i);
			p->plus_minus = parse_json_int(json, key, 0);
			snprintf(key, sizeof(key), "home_player%d_goals", i);
			p->goals = parse_json_int(json, key, 0);
			snprintf(key, sizeof(key), "home_player%d_assists", i);
			p->assists = parse_json_int(json, key, 0);
			/* No season numbers saved: start from this game. */
			snprintf(key, sizeof(key),
				 "home_player%d_season_plus_minus", i);
			p->season_plus_minus =
				parse_json_int(json, key, p->plus_minus);
			snprintf(key, sizeof(key), "home_player%d_season_goals",
				 i);
			p->season_goals = parse_json_int(json, key, p->goals);
			snprintf(key, sizeof(key),
				 "home_player%d_season_assists", i);
			p->season_assists =
				parse_json_int(json, key, p->assists);
			snprintf(key, sizeof(key), "home_player%d_pim", i);
			p->pim = clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "home_player%d_season_pim",
				 i);
			p->season_pim = clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "home_player%d_games", i);
			p->games = clamp_zero(parse_json_int(json, key, 0));
		}
		if (rcount >= 0)
			g_state.home_roster_count = rcount;
	}
	{
		int gcount = parse_json_int(json, "goalie_count", -1);
		if (gcount > SCOREBOARD_MAX_GOALIES)
			gcount = SCOREBOARD_MAX_GOALIES;
		for (int i = 0; i < gcount; i++) {
			struct scoreboard_goalie *g = &g_state.goalies[i];
			char key[64];
			snprintf(key, sizeof(key), "goalie%d_number", i);
			g->number = parse_json_int(json, key, 0);
			snprintf(key, sizeof(key), "goalie%d_sa", i);
			g->shots_against = clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "goalie%d_ga", i);
			g->goals_against = clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "goalie%d_season_sa", i);
			g->season_shots_against =
				clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "goalie%d_season_ga", i);
			g->season_goals_against =
				clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "goalie%d_games", i);
			g->games = clamp_zero(parse_json_int(json, key, 0));
			snprintf(key, sizeof(key), "goalie%d_played", i);
			g->played = parse_json_bool(json, key, false);
		}
		if (gcount >= 0) {
			g_state.goalie_count = gcount;
			g_state.has_goalie_in_net = false;
			scoreboard_set_goalie_in_net(
				parse_json_int(json, "goalie_in_net", -1));
		}
		g_state.away_goal_ends_penalty = parse_json_bool(
			json, "away_goal_ends_penalty",
			g_state.away_goal_ends_penalty);
	}
	/* Loaded totals have no matching goal history to reverse. */
	g_state.pm_event_count = 0;

	{
		int lcount = parse_json_int(json, "period_label_count", -1);
		if (lcount > 0) {
			if (lcount > SCOREBOARD_MAX_PERIOD_LABELS)
				lcount = SCOREBOARD_MAX_PERIOD_LABELS;
			g_state.period_label_count = lcount;
			for (int i = 0; i < lcount; i++) {
				char key[32];
				snprintf(key, sizeof(key), "period_label%d",
					 i);
				parse_json_string(
					json, key, g_state.period_labels[i],
					SCOREBOARD_PERIOD_LABEL_SIZE);
			}
		}
	}

	free(json);
	mark_dirty();
	return true;
}

/* ---- game management ---- */

void scoreboard_new_game(void)
{
	/* Only remember a game that had something in it. */
	if (g_state.home_score != 0 || g_state.away_score != 0 ||
	    g_state.home_shots != 0 || g_state.away_shots != 0 ||
	    g_state.home_faceoffs != 0 || g_state.away_faceoffs != 0 ||
	    g_state.game_ended)
		prev_game_save();
	g_state.game_ended = false;
	g_state.ended_player_count = 0;
	g_state.ended_goalie_count = 0;
	g_state.home_score = 0;
	g_state.away_score = 0;
	g_state.home_shots = 0;
	g_state.away_shots = 0;
	g_state.home_faceoffs = 0;
	g_state.away_faceoffs = 0;
	g_state.home_fouls = 0;
	g_state.away_fouls = 0;
	g_state.home_fouls2 = 0;
	g_state.away_fouls2 = 0;
	g_state.period = 1;
	g_state.clock_running = false;

	for (int i = 0; i < SCOREBOARD_PENALTY_SLOTS; i++) {
		g_state.home_penalties[i].active = false;
		g_state.home_penalties[i].player_number = 0;
		g_state.home_penalties[i].remaining_tenths = 0;
		g_state.home_penalties[i].phase2_tenths = 0;
		g_state.home_penalties[i].major = false;
		g_state.away_penalties[i].active = false;
		g_state.away_penalties[i].player_number = 0;
		g_state.away_penalties[i].remaining_tenths = 0;
		g_state.away_penalties[i].phase2_tenths = 0;
		g_state.away_penalties[i].major = false;
	}

	/* Rosters carry over; the game stats start fresh; season
   stats carry over. */
	scoreboard_roster_reset_game_stats();

	g_state.game_clock_accumulated_tenths = 0;
	g_state.game_clock_started = false;

	if (g_state.clock_direction == SCOREBOARD_CLOCK_COUNT_DOWN)
		g_state.clock_tenths = g_state.period_length * 10;
	else
		g_state.clock_tenths = 0;
	mark_dirty();
}

/* ---- CLI settings ---- */

void scoreboard_set_cli_executable(const char *path)
{
	safe_copy(g_state.cli_executable, path,
		  sizeof(g_state.cli_executable));
}

const char *scoreboard_get_cli_executable(void)
{
	return g_state.cli_executable;
}

void scoreboard_set_cli_extra_args(const char *args)
{
	safe_copy(g_state.cli_extra_args, args,
		  sizeof(g_state.cli_extra_args));
}

const char *scoreboard_get_cli_extra_args(void)
{
	return g_state.cli_extra_args;
}

/* ---- sport presets ---- */

static const char *k_sport_names[SCOREBOARD_SPORT_COUNT] = {
	"hockey", "basketball", "soccer", "football", "lacrosse", "rugby",
	"generic",
};

void scoreboard_set_sport(enum scoreboard_sport sport)
{
	if (sport < 0 || sport >= SCOREBOARD_SPORT_COUNT)
		sport = SCOREBOARD_SPORT_HOCKEY;
	const struct scoreboard_sport_preset *p = &k_sport_presets[sport];
	g_state.sport = sport;
	safe_copy(g_state.segment_name, p->segment_name,
		  sizeof(g_state.segment_name));
	g_state.segment_count = p->segment_count;
	g_state.ot_max = p->ot_max;
	g_state.has_shots = p->has_shots;
	g_state.has_faceoffs = p->has_faceoffs;
	g_state.has_penalties = p->has_penalties;
	g_state.has_fouls = p->has_fouls;
	safe_copy(g_state.foul_label, p->foul_label,
		  sizeof(g_state.foul_label));
	safe_copy(g_state.foul_label2, p->foul_label2,
		  sizeof(g_state.foul_label2));
	g_state.log_scores = p->log_scores;
	safe_copy(g_state.score_label, p->score_label,
		  sizeof(g_state.score_label));
	if (p->duration_seconds > 0)
		g_state.period_length = p->duration_seconds;
	g_state.clock_direction = p->default_direction;
	if (p->default_penalty_secs > 0)
		g_state.default_penalty_duration = p->default_penalty_secs;
	if (p->default_major_penalty_secs > 0)
		g_state.default_major_penalty_duration =
			p->default_major_penalty_secs;
	g_state.base_strength = p->base_strength;
	g_state.min_strength = p->min_strength;
	generate_default_period_labels();
	mark_dirty();
}

enum scoreboard_sport scoreboard_get_sport(void)
{
	return g_state.sport;
}

const struct scoreboard_sport_preset *scoreboard_get_sport_preset(void)
{
	return &k_sport_presets[g_state.sport];
}

const char *scoreboard_sport_name(enum scoreboard_sport sport)
{
	if (sport < 0 || sport >= SCOREBOARD_SPORT_COUNT)
		return k_sport_names[SCOREBOARD_SPORT_HOCKEY];
	return k_sport_names[sport];
}

enum scoreboard_sport scoreboard_sport_from_name(const char *name)
{
	if (name == NULL)
		return SCOREBOARD_SPORT_HOCKEY;
	for (int i = 0; i < SCOREBOARD_SPORT_COUNT; i++) {
		if (strcmp(name, k_sport_names[i]) == 0)
			return (enum scoreboard_sport)i;
	}
	return SCOREBOARD_SPORT_HOCKEY;
}

const char *scoreboard_get_segment_name(void)
{
	return g_state.segment_name;
}

bool scoreboard_get_has_shots(void)
{
	return g_state.has_shots;
}

bool scoreboard_get_has_penalties(void)
{
	return g_state.has_penalties;
}

bool scoreboard_get_has_fouls(void)
{
	return g_state.has_fouls;
}

const char *scoreboard_get_foul_label(void)
{
	return g_state.foul_label;
}

bool scoreboard_get_has_fouls2(void)
{
	return g_state.foul_label2[0] != '\0';
}

const char *scoreboard_get_foul_label2(void)
{
	return g_state.foul_label2;
}

bool scoreboard_get_log_scores(void)
{
	return g_state.log_scores;
}

const char *scoreboard_get_score_label(void)
{
	return g_state.score_label;
}

/* ---- strength (players per side) ---- */

void scoreboard_set_base_strength(int value)
{
	if (value < 0)
		value = 0;
	g_state.base_strength = value;
	mark_dirty();
}

int scoreboard_get_base_strength(void)
{
	return g_state.base_strength;
}

int scoreboard_get_min_strength(void)
{
	return g_state.min_strength;
}

static int compute_strength(int penalty_count, int fouls2)
{
	if (g_state.base_strength == 0)
		return 0;
	int reduction;
	if (g_state.sport == SCOREBOARD_SPORT_SOCCER)
		reduction = fouls2;
	else
		reduction = penalty_count < SCOREBOARD_MAX_RUNNING_PENALTIES
				    ? penalty_count
				    : SCOREBOARD_MAX_RUNNING_PENALTIES;
	int strength = g_state.base_strength - reduction;
	if (strength < g_state.min_strength)
		strength = g_state.min_strength;
	return strength;
}

int scoreboard_get_home_strength(void)
{
	return compute_strength(scoreboard_get_home_penalty_count(),
				g_state.home_fouls2);
}

int scoreboard_get_away_strength(void)
{
	return compute_strength(scoreboard_get_away_penalty_count(),
				g_state.away_fouls2);
}

void scoreboard_set_strength_label_format(const char *fmt)
{
	safe_copy(g_state.strength_label_format, fmt,
		  sizeof(g_state.strength_label_format));
	mark_dirty();
}

const char *scoreboard_get_strength_label_format(void)
{
	if (g_state.strength_label_format[0] == '\0')
		return kDefaultStrengthLabelFormat;
	return g_state.strength_label_format;
}

static size_t expand_strength_format(const char *fmt, const char *home_str,
				     const char *away_str, bool is_pp,
				     char *buf, size_t size)
{
	size_t out = 0;
	const char *p = fmt;
	while (*p != '\0' && out < size - 1) {
		if (p[0] == '{' && p[1] == '{') {
			const char *end = strstr(p + 2, "}}");
			if (end == NULL)
				break;
			const char *vs = p + 2;
			while (vs < end && *vs == ' ')
				vs++;
			const char *ve = end;
			while (ve > vs && *(ve - 1) == ' ')
				ve--;
			size_t nlen = (size_t)(ve - vs);
			if (nlen == 5 && strncmp(vs, "if_pp", 5) == 0) {
				p = end + 2;
				if (!is_pp) {
					const char *skip = p;
					while (*skip != '\0') {
						if (skip[0] == '{' &&
						    skip[1] == '{') {
							const char *se =
								strstr(skip + 2,
								       "}}");
							if (se != NULL) {
								const char *ts =
									skip +
									2;
								while (ts <
									       se &&
								       *ts ==
									       ' ')
									ts++;
								const char *te =
									se;
								while (te >
									       ts &&
								       *(te - 1) ==
									       ' ')
									te--;
								if ((size_t)(
									    te -
									    ts) ==
									    6 &&
								    strncmp(ts,
									    "end_if",
									    6) ==
									    0) {
									p = se +
									    2;
									break;
								}
							}
						}
						skip++;
					}
					if (*skip == '\0')
						p = skip;
				}
				continue;
			}
			if (nlen == 6 && strncmp(vs, "end_if", 6) == 0) {
				p = end + 2;
				continue;
			}
			const char *val = "";
			if (nlen == 4 && strncmp(vs, "home", 4) == 0)
				val = home_str;
			else if (nlen == 4 && strncmp(vs, "away", 4) == 0)
				val = away_str;
			size_t vlen = strlen(val);
			memcpy(buf + out, val, vlen);
			out += vlen;
			p = end + 2;
		} else {
			buf[out++] = *p++;
		}
	}
	while (*p != '\0' && out < size - 1)
		buf[out++] = *p++;
	buf[out] = '\0';
	return out;
}

void scoreboard_format_strength(char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	if (g_state.base_strength == 0) {
		buf[0] = '\0';
		return;
	}
	int home = scoreboard_get_home_strength();
	int away = scoreboard_get_away_strength();
	char home_str[8];
	char away_str[8];
	snprintf(home_str, sizeof(home_str), "%d", home);
	snprintf(away_str, sizeof(away_str), "%d", away);
	expand_strength_format(scoreboard_get_strength_label_format(),
			       home_str, away_str, home != away, buf, size);
}

void scoreboard_preview_strength_label(const char *fmt, char *buf, size_t size)
{
	if (buf == NULL || size == 0)
		return;
	if (fmt == NULL || fmt[0] == '\0')
		fmt = kDefaultStrengthLabelFormat;

	char even[SCOREBOARD_STRENGTH_LABEL_FORMAT_SIZE];
	expand_strength_format(fmt, "5", "5", false, even, sizeof(even));

	char pp[SCOREBOARD_STRENGTH_LABEL_FORMAT_SIZE];
	expand_strength_format(fmt, "5", "4", true, pp, sizeof(pp));

	snprintf(buf, size, "Even: %s\nPP:   %s", even, pp);
}

/* ---- action log ---- */

void scoreboard_add_action_log(const char *message)
{
	if (message == NULL)
		return;
	safe_copy(g_state.action_logs[g_state.action_log_head], message,
		  SCOREBOARD_ACTION_LOG_ENTRY_SIZE);
	g_state.action_log_head =
		(g_state.action_log_head + 1) % SCOREBOARD_ACTION_LOG_CAPACITY;
	if (g_state.action_log_count < SCOREBOARD_ACTION_LOG_CAPACITY)
		g_state.action_log_count++;
}

size_t scoreboard_copy_action_logs(char *buffer, size_t buffer_size)
{
	if (buffer == NULL || buffer_size == 0)
		return 0;
	buffer[0] = '\0';

	size_t written = 0;
	int start;
	if (g_state.action_log_count < SCOREBOARD_ACTION_LOG_CAPACITY)
		start = 0;
	else
		start = g_state.action_log_head;

	for (int i = 0; i < g_state.action_log_count; i++) {
		int idx = (start + i) % SCOREBOARD_ACTION_LOG_CAPACITY;
		size_t entry_len = strlen(g_state.action_logs[idx]);
		if (written + entry_len + 2 > buffer_size)
			break;
		if (written > 0) {
			buffer[written++] = '\n';
		}
		memcpy(buffer + written, g_state.action_logs[idx], entry_len);
		written += entry_len;
		buffer[written] = '\0';
	}

	return written;
}

/* ---- game event log ---- */

void scoreboard_event_log_clear(void)
{
	g_event_count = 0;
}

int scoreboard_event_log_add(int offset_seconds, const char *label)
{
	if (g_event_count >= SCOREBOARD_MAX_EVENTS)
		return -1;
	if (label == NULL)
		return -1;
	if (offset_seconds < 0)
		offset_seconds = 0;
	int idx = g_event_count;
	g_event_log[idx].offset_seconds = offset_seconds;
	safe_copy(g_event_log[idx].label, label, SCOREBOARD_EVENT_LABEL_SIZE);
	g_event_count++;
	return idx;
}

bool scoreboard_event_log_remove(int index)
{
	if (index < 0 || index >= g_event_count)
		return false;
	for (int i = index; i < g_event_count - 1; i++)
		g_event_log[i] = g_event_log[i + 1];
	g_event_count--;
	return true;
}

int scoreboard_event_log_find_last(const char *prefix)
{
	if (prefix == NULL)
		return -1;
	size_t len = strlen(prefix);
	if (len == 0)
		return -1;
	for (int i = g_event_count - 1; i >= 0; i--) {
		if (strncmp(g_event_log[i].label, prefix, len) == 0)
			return i;
	}
	return -1;
}

int scoreboard_event_log_count(void)
{
	return g_event_count;
}

const struct scoreboard_game_event *scoreboard_event_log_get(int index)
{
	if (index < 0 || index >= g_event_count)
		return NULL;
	return &g_event_log[index];
}

bool scoreboard_event_log_write(const char *path)
{
	if (path == NULL)
		return false;
	FILE *f = fopen(path, "w");
	if (f == NULL)
		return false;

	for (int i = 0; i < g_event_count; i++) {
		int total = g_event_log[i].offset_seconds;
		int hours = total / 3600;
		int minutes = (total % 3600) / 60;
		int seconds = total % 60;
		fprintf(f, "%d:%02d:%02d %s\n", hours, minutes, seconds,
			g_event_log[i].label);
	}

	fclose(f);
	return true;
}

bool scoreboard_event_log_file_has_content(const char *path)
{
	if (path == NULL)
		return false;
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fclose(f);
	return size > 0;
}

int scoreboard_event_log_read(const char *path)
{
	if (path == NULL)
		return -1;
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return -1;

	int loaded = 0;
	char line[256];
	while (fgets(line, sizeof(line), f) != NULL) {
		/* Strip trailing newline */
		size_t len = strlen(line);
		if (len > 0 && line[len - 1] == '\n')
			line[len - 1] = '\0';

		/* Parse "H:MM:SS label" format */
		int hours = 0, minutes = 0, seconds = 0;
		int consumed = 0;
		if (sscanf(line, "%d:%d:%d %n", &hours, &minutes, &seconds,
			   &consumed) < 3 ||
		    consumed == 0) {
			continue; /* skip malformed lines */
		}

		const char *label = line + consumed;
		if (label[0] == '\0')
			continue;

		int offset = hours * 3600 + minutes * 60 + seconds;
		if (scoreboard_event_log_add(offset, label) < 0)
			break; /* capacity reached */
		loaded++;
	}

	fclose(f);
	return loaded;
}
