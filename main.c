#define _XOPEN_SOURCE
#define _BSD_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <curl/curl.h>

char *urls[] = {
	"https://domgoergen.com/twtxt/mdom.txt",
	// "http://domgoergen.com/twtxt/8ball.txt",
	// "http://domgoergen.com/twtxt/bullseye.txt",
	NULL
};

struct tweet {
	time_t timestamp;
	char *msg;
};

struct MemoryStruct {
	char *memory;
	size_t size;
};

time_t parse_timestamp(char **c ) {
	struct tm tm;
	memset(&tm, 0, sizeof(struct tm));
	char *rest ;

	if (!(rest = strptime(*c, "%Y-%m-%d", &tm))) {
		return -1;
	}
	*c=rest;

	// skip T or ' '
	(*c)++;

	rest = strptime(*c, "%H:%M:%S", &tm);
	if ( ! rest && !(rest = strptime(*c, "%H:%M", &tm))) {
		return -1;
	}
	*c=rest;

	return mktime(&tm);
}

void parse_twtfile(char *c, size_t size)
{
	// start of line
	while (*c != 0) {
		if ( *(c+10) == ' ' ) {
			*(c+10) = 'T';
		}

		struct tweet *t = malloc(sizeof(struct tweet));

		t->timestamp = parse_timestamp(&c);

		assert( t->timestamp != -1 ) ;

		while ( *c == ' ' || *c == '\t' ) {
			c++;
		}

		char* start_msg = c;

		while ( *c != '\n' ) {
			c++;
		}

		size_t msg_size = c - start_msg ;

		t->msg = malloc( msg_size + 1 );
		assert(t->msg);
		memcpy(&(t->msg[0]), start_msg, msg_size );
		t->msg[msg_size] = 0;

		printf("%d %s\n", (int)t->timestamp, t->msg);

		free(t->msg);
		free(t);
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

	for (int i = 0; urls[i] != NULL; i++) {
		char *url = urls[i];
		fprintf(stderr, "%s\n", url);

		CURL *c;

		if ((c = curl_easy_init())) {
			struct MemoryStruct *chunk =
			    malloc(sizeof(struct MemoryStruct));
			chunk->memory = malloc(1);	/* will be grown as needed by the realloc above */
			chunk->size = 0;	/* no data at this point */
			/* send all data to this function  */
			curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
					 WriteMemoryCallback);

			/* we pass our 'chunk' struct to the callback function */
			curl_easy_setopt(c, CURLOPT_WRITEDATA, (void *)chunk);
			curl_easy_setopt(c, CURLOPT_PRIVATE, (void *)chunk);

			curl_easy_setopt(c, CURLOPT_URL, url);
			curl_multi_add_handle(multi_handle, c);
		}
	}

	int repeats = 0;
	/* we start some action by calling perform right away */
	curl_multi_perform(multi_handle, &still_running);

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
					fprintf(stderr,
						"We received code: %ld\n",
						code);
				}
				struct MemoryStruct *chunk;
				res =
				    curl_easy_getinfo(e, CURLINFO_PRIVATE,
						      &chunk);
				parse_twtfile(chunk->memory, chunk->size);

				curl_multi_remove_handle(multi_handle, e);
				curl_easy_cleanup(e);
			}
		}

	}
	while (still_running);

	curl_multi_cleanup(multi_handle);
	curl_global_cleanup();
	exit(0);
}
