/*  reader.c
 *  Copyright (C) 2002 Evgeny Chukreev <codedj@echo.ru>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  $Id$
 *
*/ 

#include <malloc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <dlfcn.h>
#include <string.h>

#include "AlsaPlayer.h"
#include "alsaplayer_error.h"
#include "reader.h"

#define MAX_READER_PLUGINS 10
static reader_plugin plugins [MAX_READER_PLUGINS];
static int plugin_count = 0;

static int register_plugin (reader_plugin *the_plugin)
{
	int version;
	int error_count = 0;
	reader_plugin *tmp = &plugins[plugin_count];
	
	/* check version */
	tmp->version = the_plugin->version;
	if (tmp->version) {
		if ((version = tmp->version) != READER_PLUGIN_VERSION) {
			alsaplayer_error("Wrong version number on plugin (v%d, wanted v%d)",
					version - READER_PLUGIN_BASE_VERSION,
					READER_PLUGIN_VERSION - READER_PLUGIN_BASE_VERSION);      
			return 0;
		}
	}

	memcpy (tmp, the_plugin, sizeof (reader_plugin));

	/* checks */
	if (tmp->name == NULL) {
		alsaplayer_error("No name");
		error_count++;
	}
	if (tmp->author == NULL) {
		alsaplayer_error("No author");
		error_count++;
	}	
	if (tmp->init == NULL) {
		alsaplayer_error("No init function");
		error_count++;
	}
	if (tmp->can_handle == NULL) {
		alsaplayer_error("No can_handle function");
		error_count++;
	}
	if (tmp->open == NULL) {
		alsaplayer_error("No open function");
		error_count++;
	}
	if (tmp->close == NULL) {
		alsaplayer_error("No close function");
		error_count++;
	}
	if (tmp->shutdown == NULL) {
		alsaplayer_error("No shutdown function");
		error_count++;
	}
	if (tmp->seek == NULL) {
		alsaplayer_error("No seek function");
		error_count++;
	}
	if (tmp->expand == NULL) {
		alsaplayer_error("No expand function");
		error_count++;
	}
	if (tmp->can_expand == NULL) {
		alsaplayer_error("No can_expand function");
		error_count++;
	}

	if (error_count) {
	    	alsaplayer_error("At least %d error(s) were detected", error_count);
		return 0;
	}	
	
	plugin_count++;
	
	if (global_verbose)
		alsaplayer_error("Loading reader plugin: %s", tmp->name);

	/* Initialize plugin */
	tmp->init ();

	return 1;
}

/**
 * Call this function to load reader plugins.
 * This function should be called only once.
 */
void reader_init (void)
{
    char path[1024];
    DIR *dir;
    struct stat buf;
    struct dirent *entry;
    reader_plugin_info_type reader_plugin_info;
    void *handle;
    char *ext;

    /* Initialize plugins array */
    memset (plugins, 0, sizeof(plugins));
    
    /* Trying to open plugins dir */
    if (!(dir = opendir (ADDON_DIR "/reader")))  return;
    
    /* for each entry in opened dir */
    while ((entry = readdir(dir)) != NULL) {
	/* skip .. and . entries */
	if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
	
	/* compose full plugin path */
	sprintf(path, "%s/reader/%s", ADDON_DIR, entry->d_name);
	if (stat(path, &buf)) continue;
	
	/* skip not regular files */
	if (!S_ISREG(buf.st_mode))  continue;

	/* handle only .so files */
	ext = strrchr(path, '.');
	if (!ext++)  continue; 		
	if (strcasecmp(ext, "so"))  continue;

	/* trying to load plugin */
	if ((handle = dlopen(path, RTLD_NOW |RTLD_GLOBAL))) {
	    reader_plugin_info = (reader_plugin_info_type) dlsym(handle, "reader_plugin_info");

	    if (reader_plugin_info) {
		reader_plugin *the_plugin;
	    	
		the_plugin = reader_plugin_info();
		if (the_plugin)  the_plugin->handle = handle;
	    
		if (!register_plugin(the_plugin)) {
		    alsaplayer_error("Error loading %s", path);
		    dlclose(handle);
		    continue;
		}
	    } else {
		alsaplayer_error("Could not find reader_plugin_info symbol in shared object `%s'", path);
		dlclose(handle);
	    }	
	} else {
	    alsaplayer_error("%s", dlerror());
	}
    } /* end of: while ((entry = readdir(dir)) != NULL) */

    closedir(dir);
}

/////////////////////////////////////// API for input plugins!!! /////////
extern "C" {

// Like fopen
reader_type *reader_open (const char *uri)
{
    int i = plugin_count;
    reader_plugin *plugin = plugins, *best_plugin = NULL;
    float max_q = 0.0;
    reader_type *h = (reader_type*)malloc (sizeof(reader_type));

    if (!h)  return NULL;

    // Search for best reader plugin
    for (;i--;plugin++) {
	float q = plugin->can_handle (uri);
	
	if (q == 1.0) {
	    best_plugin = plugin;
	    break;
	}
	
	if (q > max_q) {
	    max_q = q;
	    best_plugin = plugin;
	}
    }

    /* use this plugin */
    if (best_plugin) {
	h->plugin = best_plugin;
	h->fd = h->plugin->open (uri);

	/* If fail to open */
	if (h->fd == NULL) {
	    free (h);
	    return NULL;
	}
	
	return h;
    }
 
    /* First chance failed. */
    free (h);
    
    /* Second chance!!! (try treat it as a file) */
    if (strncmp (uri, "file:", 5)) {
	char new_uri [1024];

	snprintf (new_uri, 1024, "file:%s", uri);
	return reader_open (new_uri);
    }
    
    /* Couldn't find reader */
    return NULL;
}

// Like fclose
int reader_close (reader_type *h)
{
    h->plugin->close (h->fd);
    free (h);

    return 0;
}

// Like fread
size_t reader_read (void *ptr, size_t size, reader_type *h)
{
    return h->plugin->read (ptr, size, h->fd);
}

// Like fseek
int reader_seek (reader_type *h, long offset, int whence)
{
    return h->plugin->seek (h->fd, offset, whence);
}

// Like ftell
long reader_tell (reader_type *h)
{
    return h->plugin->tell (h->fd);
}
// try to expand URI.
// Function returns list of pointers to the expanded URIs.
// or NULL if this URI is not expandable....
// You should free it with the reader_free_expanded function.
char **reader_expand (const char *uri)
{
    int i = plugin_count;
    reader_plugin *plugin = plugins, *best_plugin = NULL;
    float max_q = 0.0;

    // Search for best reader plugin
    for (;i--;plugin++) {
	float q = plugin->can_expand (uri);
	
	if (q == 1.0) {
	    best_plugin = plugin;
	    break;
	}
	
	if (q > max_q) {
	    max_q = q;
	    best_plugin = plugin;
	}
    }

    /* use this plugin */
    if (best_plugin) {
	return best_plugin->expand (uri);
    }
 
    /* Second chance!!! (try treat it as a file) */
    if (strncmp (uri, "file:", 5)) {
	char new_uri [1024];

	snprintf (new_uri, 1024, "file:%s", uri);
	return reader_expand (new_uri);
    }
    
    /* Couldn't find reader */
    return NULL;
}

/**
 * Free memory allocated for expanded list
*/
void reader_free_expanded (char **list)
{
    char **uri = list;
    
    if (!list)  return;

    while (*uri)  free (*(uri++));
    free (list);
}


} /* end of: extern "C" */
