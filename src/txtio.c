#define _XOPEN_SOURCE
#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <curl/curl.h>

struct feed {
	char *url;
	char *nick;
};

struct feed *feeds[] = {
	&(struct feed){"https://domgoergen.com/twtxt/mdom.txt", "mdom"},
	&(struct feed){"http://domgoergen.com/twtxt/8ball.txt", "8ball"},
	&(struct feed){"http://domgoergen.com/twtxt/bullseye.txt", "bullseye"},
	NULL
};

struct tweet {
	time_t timestamp;
	char *msg;
	char *nick;
};

struct tweets {
	struct tweet **data;
	size_t size;
	size_t allocated;
};

struct MemoryStruct {
	char *memory;
	size_t size;
	char *nick;
};

int compare_tweets(const void *s1, const void *s2)
{
	struct tweet *t1 = (struct tweet *)s1;
	struct tweet *t2 = (struct tweet *)s2;
	return t1->timestamp - t2->timestamp;
}

struct tweets *new_array(void)
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

void parse_twtfile(char *c, size_t size, struct tweets *tweets, char *nick)
{
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
		t->nick = nick;

		add_to_array(t, tweets);

		// char buffer[6];
		// strftime(buffer, sizeof(buffer), "%H:%M", gmtime(&(t->timestamp)));

		// skip newline
		c++;
	}
}

static size_t
WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	struct MemoryStruct *mem = (struct MemoryStruct *)userp;

	mem->memory = realloc(mem->memory, mem->size + realsize + 1);
	if (mem->memory == NULL) {
		/* out of memory! */
		printf("not enough memory (realloc returned NULL)\n");
		return 0;
	}

	memcpy(&(mem->memory[mem->size]), contents, realsize);
	mem->size += realsize;
	mem->memory[mem->size] = 0;

	return realsize;
}

int main(int argc, char **argv, char **env)
{
	curl_global_init(CURL_GLOBAL_SSL);
	CURLM *multi_handle = curl_multi_init();

	int still_running = 0;

	for (int i = 0; feeds[i] != NULL; i++) {
		CURL *c;

		if ((c = curl_easy_init())) {
			struct MemoryStruct *chunk =
			    malloc(sizeof(struct MemoryStruct));
			chunk->memory = malloc(1);	/* will be grown as needed by the realloc above */
			chunk->size = 0;	/* no data at this point */
			chunk->nick = feeds[i]->nick;
			/* send all data to this function  */
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
					 WriteMemoryCallback);

			/* we pass our 'chunk' struct to the callback function */
			curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)chunk);
			curl_easy_setopt(c, CURLOPT_PRIVATE, (void *)chunk);

			curl_easy_setopt(c, CURLOPT_URL, feeds[i]->url);
			curl_multi_add_handle(multi_handle, c);
		}
	}

	int repeats = 0;
	/* we start some action by calling perform right away */
	curl_multi_perform(multi_handle, &still_running);

	struct tweets *tweets = new_array();

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
		CURLcode res;
		while ((m = curl_multi_info_read(multi_handle, &msgq)) != NULL) {
			if (m->msg == CURLMSG_DONE) {
				CURL *e = m->easy_handle;
				long code;
				res =
				    curl_easy_getinfo(e,
						      CURLINFO_RESPONSE_CODE,
						      &code);
				if (CURLE_OK == res) {
					struct MemoryStruct *chunk;
					res =
					    curl_easy_getinfo(e,
							      CURLINFO_PRIVATE,
							      &chunk);
					parse_twtfile(chunk->memory,
						      chunk->size, tweets,
						      chunk->nick);
				}

				curl_multi_remove_handle(multi_handle, e);
				curl_easy_cleanup(e);
			}
		}

	}
	while (still_running);

	curl_multi_cleanup(multi_handle);
	curl_global_cleanup();

	qsort(tweets->data, tweets->size, sizeof(struct tweet *),
	      compare_tweets);

	for (int i = 0; i < tweets->size; i++) {
		struct tweet *t = tweets->data[i];
		printf("%s - %s%s\n\n", t->nick,
		       asctime(gmtime(&(t->timestamp))), t->msg);
	}

	exit(EXIT_SUCCESS);
}
