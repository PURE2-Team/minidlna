#include <sys/stat.h>
#include <sys/event.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "event.h"
#include "log.h"
#include "monitor.h"
#include "minidlnatypes.h"
#include "upnpglobalvars.h"
#include "sql.h"
#include "utils.h"

static void
vnode_process(struct event *ev, u_int fflags)
{
	const char *path;
	char *sql, **result, tmp_path[PATH_MAX], *esc_name;
	int rows, result_path_len;
	DIR* d;
	struct dirent *entry;
	bool found_flag;

	path = (const char *)ev->data;

	if (fflags & NOTE_DELETE) {
		DPRINTF(E_DEBUG, L_INOTIFY, "Path [%s] deleted.\n", path);
		close(ev->fd);
		free(ev);
		monitor_remove_directory(0, path);
		return;
	} else if ((fflags & (NOTE_WRITE | NOTE_LINK)) ==
	    (NOTE_WRITE | NOTE_LINK)) {

		DPRINTF(E_DEBUG, L_INOTIFY, "Directory [%s] content updated\n",
		    path);
		sql = sqlite3_mprintf("SELECT PATH from DETAILS where "
		    "(PATH > '%q/' and PATH <= '%q/%c') and SIZE = ''",
		    path, path, 0xFF);
		DPRINTF(E_DEBUG, L_INOTIFY, "SQL: %s\n", sql);
		if ((sql_get_table(db, sql, &result, &rows, NULL) !=
		    SQLITE_OK)) {
			DPRINTF(E_WARN, L_INOTIFY,
			    "Read state [%s]: Query failed\n", path);
			goto err1;
		}

		for (int i = 1; i <= rows; i++) {
			DPRINTF(E_DEBUG, L_INOTIFY,
			    "Indexed content: %s\n", result[i]);
			if (access(result[i], R_OK) == -1)
				monitor_remove_directory(0, result[i]);
		}

		if ((d = opendir(path)) == NULL) {
			DPRINTF(E_ERROR, L_INOTIFY, "Can't list [%s] (%s)\n",
			    path, strerror(errno));
			goto err2;
		}

		for (entry = readdir(d); entry != NULL; entry = readdir(d)) {
			if ((entry->d_type != DT_DIR) ||
			    (strcmp(entry->d_name, "..") == 0) ||
			    (strcmp(entry->d_name, ".") == 0))
				continue;

			result_path_len = snprintf(tmp_path, PATH_MAX,
			    "%s/%s", path, entry->d_name);
			if (result_path_len >= PATH_MAX) {
				DPRINTF(E_ERROR, L_INOTIFY,
				    "File path too long for %s!",
				    entry->d_name);
				continue;
			}

			DPRINTF(E_DEBUG, L_INOTIFY, "Walking %s\n", tmp_path);
			found_flag = false;
			for (int i = 1; i <= rows; i++) {
				if (strcmp(result[i], tmp_path) == 0) {
					found_flag = true;
					break;
				}
			}
			if (!found_flag) {
				esc_name = strdup(entry->d_name);
				if (esc_name == NULL) {
					DPRINTF(E_ERROR, L_INOTIFY,
					    "strdup error");
					continue;
				}
				esc_name = modifyString(esc_name, "&", "&amp;amp;", 0);
				monitor_insert_directory(1, esc_name, tmp_path);
				free(esc_name);
			}
		}
	} else if (fflags & NOTE_WRITE) {

		DPRINTF(E_DEBUG, L_INOTIFY, "File [%s] content updated\n",
		    path);
		sql = sqlite3_mprintf("SELECT PATH from DETAILS where "
		    "(PATH > '%q/' and PATH <= '%q/%c') and SIZE <> ''",
		    path, path, 0xFF);
		if (sql_get_table(db, sql, &result, &rows, NULL) != SQLITE_OK) {
			DPRINTF(E_WARN, L_INOTIFY,
			    "Read state [%s]: Query failed\n", path);
			goto err1;
		}

		for (int i = 1; i <= rows; i++) {
			DPRINTF(E_DEBUG, L_INOTIFY,
			    "Indexed content: %s\n", result[i]);
			if (access(result[i], R_OK) == -1)
				monitor_remove_file(result[i]);
		}

		if ((d = opendir(path)) == NULL) {
			DPRINTF(E_ERROR, L_INOTIFY,
			    "Can't list [%s] (%s)\n", path, strerror(errno));
			goto err2;
		}

		for (entry = readdir(d); entry != NULL; entry = readdir(d)) {
			if ((entry->d_type != DT_REG) &&
			    (entry->d_type != DT_LNK))
				continue;

			result_path_len = snprintf(tmp_path, PATH_MAX, "%s/%s",
			    path, entry->d_name);
			if (result_path_len >= PATH_MAX) {
				DPRINTF(E_ERROR, L_INOTIFY,
				    "File path too long for %s!",
				    entry->d_name);
				continue;
			}
			DPRINTF(E_DEBUG, L_INOTIFY, "Walking %s\n", tmp_path);
			found_flag = false;
			for (int i = 1; i <= rows; i++)
				if (strcmp(result[i], tmp_path) == 0) {
					found_flag = true;
					break;
				}
			if (!found_flag ) {
				struct stat st;

				if (stat(tmp_path, &st) != 0) {
					DPRINTF(E_ERROR, L_INOTIFY,
					    "stat(%s): %s\n", tmp_path,
					    strerror(errno));
					continue;
				}
				esc_name = strdup(entry->d_name);
				if (esc_name == NULL) {
					DPRINTF(E_ERROR, L_INOTIFY,
					    "strdup error");
					continue;
				}
				esc_name = modifyString(esc_name, "&", "&amp;amp;", 0);
				if (S_ISDIR(st.st_mode))
					monitor_insert_directory(1, esc_name, tmp_path);
				else
					monitor_insert_file(esc_name, tmp_path);
				free(esc_name);
			}
		}
	} else
		return;

	closedir(d);
err2:
	sqlite3_free_table(result);
err1:
	sqlite3_free(sql);
}

int
add_watch(int fd __unused, const char *path)
{
	struct event *ev;
	int wd;

	wd = open(path, O_RDONLY);
	if (wd < 0) {
		DPRINTF(E_ERROR, L_INOTIFY, "open(%s) [%s]\n",
		    path, strerror(errno));
		return (errno);
	}

	if ((ev = malloc(sizeof(struct event))) == NULL) {
		DPRINTF(E_ERROR, L_INOTIFY, "malloc() error\n");
		close(wd);
		return (ENOMEM);
	}
	if ((ev->data = strdup(path)) == NULL) {
		DPRINTF(E_ERROR, L_INOTIFY, "strdup() error\n");
		close(wd);
		free(ev);
		return (ENOMEM);
	}
	ev->fd = wd;
	ev->rdwr = EVENT_VNODE;
	ev->process_vnode = vnode_process;

	DPRINTF(E_DEBUG, L_INOTIFY, "kqueue add_watch [%s]\n", path);
	event_module.add(ev);

	return (0);
}

/*
 * XXXGL: this function has too much copypaste of inotify_create_watches().
 * We need to split out inotify stuff from monitor.c into monitor_inotify.c,
 * compile the latter on Linux and this file on FreeBSD, and keep monitor.c
 * itself platform independent.
 */
void
kqueue_monitor_start()
{
	struct media_dir_s *media_path;
	char **result;
	int rows;

	DPRINTF(E_DEBUG, L_INOTIFY, "kqueue monitoring starting\n");
	for (media_path = media_dirs; media_path != NULL;
	    media_path = media_path->next)
		add_watch(0, media_path->path);
	sql_get_table(db, "SELECT PATH from DETAILS where MIME is NULL and PATH is not NULL", &result, &rows, NULL);
	for (int i = 1; i <= rows; i++ )
		add_watch(0, result[i]);
	sqlite3_free_table(result);
}