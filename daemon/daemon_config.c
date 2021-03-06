/*

Copyright (c) 2017-2018, Feral Interactive
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
 * Neither the name of Feral Interactive nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

 */
#define _GNU_SOURCE

#include "daemon_config.h"
#include "logging.h"

/* Ben Hoyt's inih library */
#include "ini.h"

#include <linux/limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Name and possible location of the config file */
#define CONFIG_NAME "gamemode.ini"
#define CONFIG_DIR "/usr/share/gamemode/"

/* Default value for the reaper frequency */
#define DEFAULT_REAPER_FREQ 5

/**
 * The config holds various details as needed
 * and a rwlock to allow config_reload to be called
 */
struct GameModeConfig {
	pthread_rwlock_t rwlock;
	int inotfd;
	int inotwd;

	char whitelist[CONFIG_LIST_MAX][CONFIG_VALUE_MAX];
	char blacklist[CONFIG_LIST_MAX][CONFIG_VALUE_MAX];

	char startscripts[CONFIG_LIST_MAX][CONFIG_VALUE_MAX];
	char endscripts[CONFIG_LIST_MAX][CONFIG_VALUE_MAX];

	long reaper_frequency;
};

/*
 * Add values to a char list
 */
static bool append_value_to_list(const char *list_name, const char *value,
                                 char list[CONFIG_LIST_MAX][CONFIG_VALUE_MAX])
{
	unsigned int i = 0;
	while (*list[i] && ++i < CONFIG_LIST_MAX)
		;

	if (i < CONFIG_LIST_MAX) {
		strncpy(list[i], value, CONFIG_VALUE_MAX);

		if (list[i][CONFIG_VALUE_MAX - 1] != '\0') {
			LOG_ERROR("Config: Could not add [%s] to [%s], exceeds length limit of %d\n",
			          value,
			          list_name,
			          CONFIG_VALUE_MAX);

			memset(list[i], 0, sizeof(list[i]));
			return false;
		}
	} else {
		LOG_ERROR("Config: Could not add [%s] to [%s], exceeds number of %d\n",
		          value,
		          list_name,
		          CONFIG_LIST_MAX);
		return false;
	}

	return true;
}

/*
 * Get a positive long value from a string
 */
static bool get_long_value(const char *value_name, const char *value, long *output)
{
	char *end = NULL;
	long config_value = strtol(value, &end, 10);

	if (errno == ERANGE) {
		LOG_ERROR("Config: %s overflowed, given [%s]\n", value_name, value);
		return false;
	} else if (config_value <= 0 || !(*value != '\0' && end && *end == '\0')) {
		LOG_ERROR("Config: %s was invalid, given [%s]\n", value_name, value);
		return false;
	} else {
		*output = config_value;
	}

	return true;
}

/*
 * Handler for the inih callback
 */
static int inih_handler(void *user, const char *section, const char *name, const char *value)
{
	GameModeConfig *self = (GameModeConfig *)user;
	bool valid = false;

	if (strcmp(section, "filter") == 0) {
		/* Filter subsection */
		if (strcmp(name, "whitelist") == 0) {
			valid = append_value_to_list(name, value, self->whitelist);
		} else if (strcmp(name, "blacklist") == 0) {
			valid = append_value_to_list(name, value, self->blacklist);
		}
	} else if (strcmp(section, "general") == 0) {
		/* General subsection */
		if (strcmp(name, "reaper_freq") == 0) {
			valid = get_long_value(name, value, &self->reaper_frequency);
		}
	} else if (strcmp(section, "custom") == 0) {
		/* Custom subsection */
		if (strcmp(name, "start") == 0) {
			valid = append_value_to_list(name, value, self->startscripts);
		} else if (strcmp(name, "end") == 0) {
			valid = append_value_to_list(name, value, self->endscripts);
		}
	}

	if (!valid) {
		/* Simply ignore the value, but with a log */
		LOG_MSG("Config: Value ignored [%s] %s=%s\n", section, name, value);
	}

	return 1;
}

/*
 * Load the config file
 */
static void load_config_file(GameModeConfig *self)
{
	/* Take the write lock for the internal data */
	pthread_rwlock_wrlock(&self->rwlock);

	/* Clear our config values */
	memset(self->whitelist, 0, sizeof(self->whitelist));
	memset(self->blacklist, 0, sizeof(self->blacklist));
	memset(self->startscripts, 0, sizeof(self->startscripts));
	memset(self->endscripts, 0, sizeof(self->endscripts));
	self->reaper_frequency = DEFAULT_REAPER_FREQ;

	/* try locally first */
	FILE *f = fopen(CONFIG_NAME, "r");
	if (!f) {
		f = fopen(CONFIG_DIR CONFIG_NAME, "r");
		if (!f) {
			/* Failure here isn't fatal */
			LOG_ERROR("Note: No config file found [%s] in working directory or in [%s]\n",
			          CONFIG_NAME,
			          CONFIG_DIR);
		}
	}

	if (f) {
		int error = ini_parse_file(f, inih_handler, (void *)self);

		/* Failure here isn't fatal */
		if (error) {
			LOG_MSG("Failed to parse config file - error on line %d!\n", error);
		}

		fclose(f);
	}

	/* Release the lock */
	pthread_rwlock_unlock(&self->rwlock);
}

/*
 * Create a context object
 */
GameModeConfig *config_create(void)
{
	GameModeConfig *newconfig = (GameModeConfig *)malloc(sizeof(GameModeConfig));

	return newconfig;
}

/*
 * Initialise the config
 */
void config_init(GameModeConfig *self)
{
	pthread_rwlock_init(&self->rwlock, NULL);

	/* load the initial config */
	load_config_file(self);
}

/*
 * Re-load the config file
 */
void config_reload(GameModeConfig *self)
{
	load_config_file(self);
}

/*
 * Destroy the config
 */
void config_destroy(GameModeConfig *self)
{
	pthread_rwlock_destroy(&self->rwlock);

	/* Finally, free the memory */
	free(self);
}

/*
 * Checks if the client is whitelisted
 */
bool config_get_client_whitelisted(GameModeConfig *self, const char *client)
{
	/* Take the read lock for the internal data */
	pthread_rwlock_rdlock(&self->rwlock);

	/* If the whitelist is empty then everything passes */
	bool found = true;
	if (self->whitelist[0][0]) {
		/*
		 * Check if the value is found in our whitelist
		 * Currently is a simple strstr check, but could be modified for wildcards etc.
		 */
		found = false;
		for (unsigned int i = 0; i < CONFIG_LIST_MAX && self->whitelist[i][0]; i++) {
			if (strstr(client, self->whitelist[i])) {
				found = true;
			}
		}
	}

	/* release the lock */
	pthread_rwlock_unlock(&self->rwlock);
	return found;
}

/*
 * Checks if the client is blacklisted
 */
bool config_get_client_blacklisted(GameModeConfig *self, const char *client)
{
	/* Take the read lock for the internal data */
	pthread_rwlock_rdlock(&self->rwlock);

	/*
	 * Check if the value is found in our whitelist
	 * Currently is a simple strstr check, but could be modified for wildcards etc.
	 */
	bool found = false;
	for (unsigned int i = 0; i < CONFIG_LIST_MAX && self->blacklist[i][0]; i++) {
		if (strstr(client, self->blacklist[i])) {
			found = true;
		}
	}

	/* release the lock */
	pthread_rwlock_unlock(&self->rwlock);
	return found;
}

/*
 * Gets the reaper frequency
 */
long config_get_reaper_thread_frequency(GameModeConfig *self)
{
	long value;
	/* Take the read lock */
	pthread_rwlock_rdlock(&self->rwlock);

	value = self->reaper_frequency;

	/* release the lock */
	pthread_rwlock_unlock(&self->rwlock);
	return value;
}

/*
 * Get a set of scripts to call when gamemode starts
 */
void config_get_gamemode_start_scripts(GameModeConfig *self,
                                       char scripts[CONFIG_LIST_MAX][CONFIG_VALUE_MAX])
{
	/* Take the read lock */
	pthread_rwlock_rdlock(&self->rwlock);

	memcpy(scripts, self->startscripts, sizeof(self->startscripts));

	/* release the lock */
	pthread_rwlock_unlock(&self->rwlock);
}

/*
 * Get a set of scripts to call when gamemode ends
 */
void config_get_gamemode_end_scripts(GameModeConfig *self,
                                     char scripts[CONFIG_LIST_MAX][CONFIG_VALUE_MAX])
{
	/* Take the read lock */
	pthread_rwlock_rdlock(&self->rwlock);

	memcpy(scripts, self->endscripts, sizeof(self->startscripts));

	/* release the lock */
	pthread_rwlock_unlock(&self->rwlock);
}
