#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE
#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <libgen.h>		// basename
#include <errno.h>
#include <ctype.h> // iscntrl

#include "mkdir.h"
#include "uthash/utstring.h"
#include "uthash/utarray.h"

char *time_format = "%Y-%m-%d %H:%S";
char *pager_cmd = "less -R";
int use_pager = 1;

struct feed {
	char *url;
	char *nick;
	UT_string *content;
	long last_modified;
};

void feed_free(struct feed *feed)
{
	free(feed->url);
	free(feed->nick);
	free(feed->content);
	free(feed);
}

struct tweet {
	time_t timestamp;
	char *msg;
	char *nick;
};

void tweet_free(struct tweet *tweet)
{
	free(tweet->msg);
	free(tweet->nick);
	free(tweet);
}

struct tweets {
	struct tweet **data;
	size_t size;
	size_t allocated;
};

int tweets_compare(const void *s1, const void *s2)
{
	struct tweet *t1 = *(struct tweet **)s1;
	struct tweet *t2 = *(struct tweet **)s2;
	time_t d = t2->timestamp - t1->timestamp;
	return d == 0 ? 0 : d < 0 ? -1 : 1;
}

time_t parse_timestamp(char **c)
{
	struct tm tm;
	memset(&tm, 0, sizeof(struct tm));
	char *rest;

	rest = strptime(*c, "%Y-%m-%d", &tm);
	if (!rest) {
		return -1;
	}
	*c = rest;

	// skip T or ' '
	if (**c && (**c == 'T' || **c == ' ')) {
		(*c)++;
	}

	rest = strptime(*c, "%H:%M:%S", &tm);
	if (!rest && !(rest = strptime(*c, "%H:%M", &tm))) {
		return -1;
	}
	*c = rest;

	// TODO eval microseconds and timezone
	while (**c != ' ' && **c != '\t') {
		(*c)++;
	}

	return mktime(&tm);
}

void skip_line(char **c)
{
	for (char *i = *c; *i && *i != '\n'; i++) ;
}

void parse_twtfile(struct feed *feed, UT_array * tweets)
{
	char *c = utstring_body(feed->content);
	while (*c) {

		time_t timestamp = parse_timestamp(&c);

		if (timestamp == -1) {
			while (*c && *c++ != '\n') ;
			continue;
		}


		while (*c && (*c == '\t' || *c == ' ')) {
			c++;
		}

		char *start_msg = c;

		while (*c && *c != '\n') {
			if ( iscntrl(*c) ) {
				*c = ' ';
			}
			c++;
		}

		size_t msg_size = c - start_msg;

		struct tweet *t = malloc(sizeof(struct tweet));

		if (!t)
			oom();

		t->msg = malloc(msg_size + 1);

		if (!t->msg)
			oom();

		memcpy(&(t->msg[0]), start_msg, msg_size);
		t->msg[msg_size] = 0;
		t->timestamp = timestamp;
		t->nick = feed->nick;

		utarray_push_back(tweets, &t);

		// skip newline
		c++;
	}
}

static size_t
feed_add_content(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct feed *feed = (struct feed *)userp;
	utstring_bincpy(feed->content, contents, realsize);
	return realsize;
}

void feed_process(CURL * e, UT_array * tweets)
{
	CURLcode res;
	long code;
	res = curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
	if (res != CURLE_OK)
		return;

	struct feed *feed;

	switch (code) {
	case 0:
	case 200:
		//TODO check res!
		res = curl_easy_getinfo(e, CURLINFO_PRIVATE, &feed);
		res = curl_easy_getinfo(e,
					CURLINFO_FILETIME,
					&(feed->last_modified));
		parse_twtfile(feed, tweets);
		break;
	}
}

UT_array *feeds_get(UT_array * feeds)
{
	curl_global_init(CURL_GLOBAL_SSL);
	CURLM *multi_handle = curl_multi_init();

	int still_running = 0;

	struct feed **p = NULL;

	while ((p = (struct feed **)utarray_next(feeds, p))) {
		struct feed *feed = *p;
		CURL *c;

		if ((c = curl_easy_init())) {

			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
					 feed_add_content);
			curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)feed);
			curl_easy_setopt(c, CURLOPT_PRIVATE, (void *)feed);
			curl_easy_setopt(c, CURLOPT_URL, feed->url);
			curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1);
			curl_easy_setopt(c, CURLOPT_FILETIME, 1);
			curl_easy_setopt(c, CURLOPT_USERAGENT, "txtio/1.0");

			curl_multi_add_handle(multi_handle, c);
		}
	}

	int repeats = 0;
	/* we start some action by calling perform right away */
	curl_multi_perform(multi_handle, &still_running);

	UT_array *tweets;

	utarray_new(tweets, &ut_ptr_icd);

	do {
		CURLMcode mc;
		int numfds;

		/* wait for activity, timeout or "nothing" */
		mc = curl_multi_wait(multi_handle, NULL, 0, 1000, &numfds);

		if (mc != CURLM_OK) {
			fprintf(stderr, "%s\n", curl_multi_strerror(mc));
			break;
		}

		/* 'numfds' being zero means either a timeout or no file descriptors to
		 * wait for. Try timeout on first occurrence, then assume no file
		 * descriptors and no file descriptors to wait for means wait for 100
		 * milliseconds. */

		if (!numfds) {
			repeats++;	/* count number of repeated zero numfds */
			if (repeats > 1) {
				struct timeval wait = { 0, 100000 };
				(void)select(0, NULL, NULL, NULL, &wait);

			}
		} else {
			repeats = 0;
		}

		mc = curl_multi_perform(multi_handle, &still_running);

		int msgq = 0;
		struct CURLMsg *m;
		while ((m = curl_multi_info_read(multi_handle, &msgq)) != NULL) {
			if (m->msg == CURLMSG_DONE) {
				CURL *e = m->easy_handle;
				feed_process(e, tweets);
				curl_multi_remove_handle(multi_handle, e);
				curl_easy_cleanup(e);

			}

		}

	}
	while (still_running);

	curl_multi_cleanup(multi_handle);
	curl_global_cleanup();
	return tweets;
}

void tweets_sort(UT_array * tweets)
{
	utarray_sort(tweets, tweets_compare);
}

void tweets_display(UT_array * tweets)
{

	FILE *pager = stdout;
	if (use_pager) {
		pager = popen(pager_cmd, "w");
	}

	struct tweet **tweet = NULL;

	while ((tweet = (struct tweet **)utarray_next(tweets, tweet))) {

		struct tweet *t = *tweet;

		//TODO fix hard limit on timestamp
		time_t d = t->timestamp;
		fprintf(pager, "%d\n", (int)d);
		char timestamp[50];
		int s = strftime(timestamp, sizeof(timestamp), time_format,
				 localtime(&d));

		if (!s) {
			continue;
		}

		fprintf(pager, "* %s (%s)\n%s\n\n", t->nick, timestamp, t->msg);
	}

	fclose(pager);
}

int sql_do(sqlite3 * db, const char *sql)
{
	return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

void database_create(const char *filename)
{
	sqlite3 *db;
	int rc = sqlite3_open(filename, &db);

	if (rc == SQLITE_OK) {
		sql_do(db,
		       "create table followings"
		       "(nick text unique, url text unique, last_modified )");
	}

	sqlite3_close(db);
}

int timeline(const char *filename)
{

	sqlite3 *db;
	int rc = EXIT_SUCCESS;
	// char *err_msg;

	sqlite3_stmt *stmt;

	rc = sqlite3_open(filename, &db);
	if (rc != SQLITE_OK) {
		return EXIT_FAILURE;
	}

	rc = sqlite3_prepare_v2(db, "select nick, url from followings", -1,
				&stmt, NULL);

	if (rc != SQLITE_OK) {
		return EXIT_FAILURE;
	}

	UT_array *feeds;
	utarray_new(feeds, &ut_ptr_icd);

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		struct feed *feed = malloc(sizeof(struct feed));
		if (!feed) {
			exit(EXIT_FAILURE);
		}
		feed->nick = strdup((const char *)sqlite3_column_text(stmt, 0));
		feed->url = strdup((const char *)sqlite3_column_text(stmt, 1));
		utstring_new(feed->content);

		utarray_push_back(feeds, &feed);
	}

	sqlite3_finalize(stmt);

	UT_array *tweets = feeds_get(feeds);
	tweets_sort(tweets);
	tweets_display(tweets);
	return rc;
}

int follow(const char *filename, const char *nick, const char *url)
{
	sqlite3 *db;
	int rc = EXIT_SUCCESS;
	char *err_msg = NULL;

	rc = sqlite3_open(filename, &db);
	if (rc != SQLITE_OK) {
		return EXIT_FAILURE;
	}

	char *query =
	    sqlite3_mprintf
	    ("insert or replace into followings values ('%q', '%q', 0);", nick,
	     url);

	rc = sqlite3_exec(db, query, NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", err_msg);
		rc = EXIT_FAILURE;
	}

	sqlite3_free(err_msg);
	sqlite3_free(query);
	sqlite3_close(db);

	return rc;
}

int main(int argc, char **argv, char **env)
{
	UT_string *db_file;
	utstring_new(db_file);

	char *xdg_home = getenv("XDG_CONFIG_HOME");

	if (xdg_home) {
		utstring_printf(db_file, strdup(xdg_home));
	} else {
		utstring_printf(db_file, "%s/%s", strdup(getenv("HOME")),
				strdup(".config"));
	}

	utstring_printf(db_file, "%s/%s", strdup(basename(argv[0])),
			strdup("db.sqlite"));

	if (mkdir_p(dirname(strdup(utstring_body(db_file)))) != 0) {
		fprintf(stderr, "%s: %s\n", basename(argv[0]), strerror(errno));
		exit(EXIT_FAILURE);
	}

	database_create(utstring_body(db_file));

	if (argc == 1) {
		fprintf(stderr, "%s: Missing subcommand\n", argv[0]);
		exit(1);
	}

	if (strcmp(argv[1], "timeline") == 0) {
		timeline(utstring_body(db_file));

	} else if (strcmp(argv[1], "follow") == 0) {
		follow(utstring_body(db_file), argv[2], argv[3]);
	} else if (strcmp(argv[1], "view") == 0) {
		UT_array *feeds;
		utarray_new(feeds, &ut_ptr_icd);

		struct feed *feed = malloc(sizeof(struct feed));
		if (!feed) {
			exit(EXIT_FAILURE);
		}
		feed->nick = strdup(argv[2]);
		feed->url = strdup(argv[3]);
		utstring_new(feed->content);
		utarray_push_back(feeds, &feed);

		UT_array *tweets = feeds_get(feeds);
		tweets_sort(tweets);
		tweets_display(tweets);
	} else {

		fprintf(stderr, "%s: Unknown subcommand \"%s\"\n", argv[0],
			argv[1]);
		exit(EXIT_FAILURE);

	}

	utstring_free(db_file);

	exit(EXIT_SUCCESS);
}
