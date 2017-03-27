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

#include "asprintf/asprintf.h"
#include "mkdir.h"

char *time_format = "%y-%m-%d %H:%S";
char *pager_cmd = "less -R";
int use_pager = 1;

struct feed {
	char *url;
	char *nick;
	char *content;
	size_t size;
	long last_modified;
};

void feed_free(struct feed *feed)
{
	free(feed->url);
	free(feed->nick);
	free(feed->content);
	free(feed);
}

struct feed *feeds[] = {
	&(struct feed){
		       .url = "http://www.domgoergen.com/twtxt/mdom.txt",
		       .nick = "mdom"},
	&(struct feed){
		       .url = "http://domgoergen.com/twtxt/8ball.txt",
		       .nick = "8ball"},
	&(struct feed){
		       .url = "http://domgoergen.com/twtxt/bullseye.txt",
		       .nick = "bullseye"},
	NULL
};

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

void tweets_free(struct tweets *tweets)
{
	for (size_t i = 0; i < tweets->size; i++) {
		tweet_free(tweets->data[i]);
	}
	free(tweets);
}

int tweets_compare(const void *s1, const void *s2)
{
	struct tweet *t1 = *(struct tweet **)s1;
	struct tweet *t2 = *(struct tweet **)s2;
	time_t d = t2->timestamp - t1->timestamp;
	return d == 0 ? 0 : d < 0 ? -1 : 1;
}

struct tweets *tweets_new(void)
{
	struct tweets *t = malloc(sizeof(struct tweets));
	t->data = malloc(100 * sizeof(struct tweet *));
	t->size = 0;
	t->allocated = 100;
	return t;
}

int add_to_array(struct tweet *t, struct tweets *a)
{
	if (a->size == a->allocated) {
		a->allocated *= 2;
		void *tmp =
		    realloc(a->data, (a->allocated * sizeof(struct tweet *)));
		if (!tmp) {
			fprintf(stderr, "ERROR: Couldn't realloc memory!\n");
			return (-1);
		}
		a->data = tmp;
	}

	a->data[a->size] = t;
	a->size++;
	return a->size;
}

time_t parse_timestamp(char **c)
{
	struct tm tm;
	memset(&tm, 0, sizeof(struct tm));
	char *rest;

	if (!(rest = strptime(*c, "%Y-%m-%d", &tm))) {
		return -1;
	}
	*c = rest;

	// skip T or ' '
	(*c)++;

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

void parse_twtfile(struct feed *feed, struct tweets *tweets)
{
	char *c = feed->content;
	// TODO use size to check that i don't leave c
	// start of line
	while (*c != 0) {

		struct tweet *t = malloc(sizeof(struct tweet));

		t->timestamp = parse_timestamp(&c);

		assert(t->timestamp != -1);

		while (*c == ' ' || *c == '\t') {
			c++;
		}

		char *start_msg = c;

		while (*c != '\n') {
			c++;
		}

		size_t msg_size = c - start_msg;

		t->msg = malloc(msg_size + 1);
		assert(t->msg);
		memcpy(&(t->msg[0]), start_msg, msg_size);
		t->msg[msg_size] = 0;
		t->nick = feed->nick;

		add_to_array(t, tweets);

		// char buffer[6];
		// strftime(buffer, sizeof(buffer), "%H:%M", gmtime(&(t->timestamp)));

		// skip newline
		c++;
	}
}

static size_t
feed_add_content(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct feed *feed = (struct feed *)userp;

	feed->content = realloc(feed->content, feed->size + realsize + 1);
	if (feed->content == NULL) {
		/* out of memory! */
		printf("not enough memory (realloc returned NULL)\n");
		return 0;
	}

	memcpy(&(feed->content[feed->size]), contents, realsize);
	feed->size += realsize;
	feed->content[feed->size] = 0;

	return realsize;
}

void feed_process(CURL * e, struct tweets *tweets)
{
	CURLcode res;
	long code;
	res = curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
	if (res != CURLE_OK)
		return;

	struct feed *feed;

	switch (code) {
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

struct tweets *feeds_get(struct feed *feeds[])
{
	curl_global_init(CURL_GLOBAL_SSL);
	CURLM *multi_handle = curl_multi_init();

	int still_running = 0;

	for (int i = 0; feeds[i] != NULL; i++) {
		struct feed *feed = feeds[i];
		CURL *c;

		if ((c = curl_easy_init())) {
			feed->content = malloc(1);
			feed->size = 0;

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

	struct tweets *tweets = tweets_new();

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

void tweets_sort(struct tweets *tweets)
{
	qsort(tweets->data, tweets->size, sizeof(struct tweet *),
	      tweets_compare);
}

void tweets_display(struct tweets *tweets)
{

	FILE *pager = stdout;
	if (use_pager) {
		pager = popen(pager_cmd, "w");
	}

	for (int i = 0; i < tweets->size; i++) {
		struct tweet *t = tweets->data[i];

		//TODO fix hard limit on timestamp
		char timestamp[50];
		int s = strftime(timestamp, sizeof(timestamp), time_format,
				 localtime(&(t->timestamp)));

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

int follow(const char *filename, const char *nick, const char *url)
{
	sqlite3 *db;
	int rc = SQLITE_OK;
	char *err_msg = NULL;

	rc = sqlite3_open(filename, &db);
	if (rc != SQLITE_OK) {
		return rc;
	}

	char *query =
	    sqlite3_mprintf
	    ("insert or replace into followings values ('%q', '%q', 0);", nick,
	     url);

	rc = sqlite3_exec(db, query, NULL, NULL, &err_msg);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s\n", err_msg);
		rc = -1;
	}

	sqlite3_free(err_msg);
	sqlite3_free(query);
	sqlite3_close(db);

	return rc;
}

int main(int argc, char **argv, char **env)
{
	char *db_file;
	char *config_dir = getenv("XDG_CONFIG_HOME");
	if (!config_dir) {
		if (asprintf(&config_dir, "%s/%s", getenv("HOME"), ".config") ==
		    -1) {
			fprintf(stderr, "Can't allocate memory!\n");
			exit(EXIT_FAILURE);
		}
	}

	if (asprintf
	    (&db_file, "%s/%s/%s", config_dir, basename(argv[0]),
	     strdup("db.sqlite")) == -1) {
		fprintf(stderr, "Can't allocate memory!");
		exit(EXIT_FAILURE);
	}

	if (mkdir_p(dirname(strdup(db_file))) != 0) {
		fprintf(stderr, "%s: %s\n", basename(argv[0]), strerror(errno));
		exit(EXIT_FAILURE);
	}

	database_create(db_file);

	if (argc == 1) {
		fprintf(stderr, "%s: Missing subcommand\n", argv[0]);
		exit(1);
	}

	if (strcmp(argv[1], "timeline") == 0) {

		struct tweets *tweets = feeds_get(feeds);
		tweets_sort(tweets);
		tweets_display(tweets);

		tweets_free(tweets);

	} else if (strcmp(argv[1], "follow") == 0) {

		follow(db_file, argv[2], argv[3]);

	} else {

		fprintf(stderr, "%s: Unknown subcommand \"%s\"\n", argv[0],
			argv[1]);
		exit(EXIT_FAILURE);

	}

	free(db_file);

	exit(EXIT_SUCCESS);
}
