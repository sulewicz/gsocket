/*
 * For GS-NETCAT.
 * 
 * Monitor various aspect of the system (such as new user login
 * and idle time of users).
 */


#include "common.h"
#include "utils.h"

struct utmp_db_user
{
	char user[UT_NAMESIZE];
	int idle;
	int idle_old;
	int token;
};

// struct GS_BUF
// {
// 	void *data;
// 	size_t last;
// 	size_t total_len;
// };

// int
// GS_BUF_add(struct GS_BUF *gsb, void *data, size_t len)
// {
// 	if (gsb->total_len - gsb->last < len)
// 	{
// 		gsb->total_len += len * 10;
// 		gsb->data = realloc(gsb->data, gsb->total_len);
// 	}

// 	memcpy((uint8_t *)gsb->data + gsb->last, data, len);
// 	gsb->last += len;

// 	return 0;
// }

// struct GS_BUF mon_db;

GS_LIST udb;
static int is_udb_init;

static int
utmp_db_find(const char *needle, struct utmp_db_user **uret)
{
	GS_LIST_ITEM *li = NULL;

	while (1)
	{
		li = GS_LIST_next(&udb, li);
		if (li == NULL)
			break;

		struct utmp_db_user *u = (struct utmp_db_user *)li->data;
		if (strcmp(u->user, needle) != 0)
			continue;

		// User Name matches the needle
		*uret = u;
		return 0;
	}

	*uret = NULL;
	return -1; // User Name not found
}

static struct utmp_db_user *
utmp_db_add(const char *user, int idle, int token)
{
	struct utmp_db_user *new;

	DEBUGF_C("Adding new user %s with idle %d\n", user, idle);
	new = malloc(sizeof *new);
	new->idle = idle;
	new->idle_old = 0;
	new->token = token;
	snprintf(new->user, sizeof new->user, "%s", user);

	GS_LIST_add(&udb, NULL, new, new->idle);

	return new;
}

/*
 */
void
GS_IDS_utmp_free(void)
{
	GS_LIST_ITEM *li = NULL;

	while (1)
	{
		li = GS_LIST_next(&udb, NULL);
		if (li == NULL)
			break;

		XFREE(li->data);
		GS_LIST_del(li);
	}

	is_udb_init = 0;
}

// When to consider a user transitioning from IDLE to not IDLE
#ifdef DEBUG
#define IDLE_THRESHOLD		(10)
#else
#define IDLE_THRESHOLD		(60 * 60)  // 1h
#endif

/*
 * Call every second.
 * Compare DB from memory with utmp file.
 *
 * Find any new user.
 * Find any known user that is no longer idle.
 * Find least idle user.
 */
void
GS_IDS_utmp(GS_LIST *new_login, GS_LIST *new_active, char **least_idle, int *sec_idle)
{
	struct utmpx *ut;
	int idle;
	int ret;
	struct stat s;
	char buf[128];
	int token = gopt.tv_now.tv_sec;

	if (is_udb_init == 0)
	{
		GS_LIST_init(&udb, 0);
	}
	*least_idle = NULL;
	*sec_idle = INT_MAX;

	gettimeofday(&gopt.tv_now, NULL);
	setutxent();
	while ((ut = getutxent()) != NULL)
	{
		if (ut->ut_type != USER_PROCESS)
			continue;
		ut->ut_user[UT_NAMESIZE - 1] = 0x00;  // be sure for strcmp...

		// Get idle time of the user's tty
		snprintf(buf, sizeof buf, "/dev/%s", ut->ut_line);
		stat(buf, &s);
		idle = MAX(0, gopt.tv_now.tv_sec - s.st_atime);

		struct utmp_db_user *u;
		ret = utmp_db_find(ut->ut_user, &u);
		if (ret != 0)
		{
			// NOT found. Add user.
			u = utmp_db_add(ut->ut_user, idle, token);
			if (is_udb_init != 0)
			{
				DEBUGF("New Login detected\n");
				GS_LIST_add(new_login, NULL, ut->ut_user, 0);
			}
		} else {
			// Update idle if this is a new run over utmp.
			// Otherwise u->idle will never get larger when user
			// idles. Slower method would be to set all records
			// u->idle to INT_MAX before while loop.
			if (u->token != token)
			{
				u->token = token;
				u->idle = idle;
			}
			// Update current user's idle if lower.
			if (idle < u->idle)
			{
				// DEBUGF_W("Updating idle to %d of user %s\n", idle, u->user);
				u->idle = idle;
			}
		}
		XASSERT(u != NULL, "utmp entry is NULL\n");

		if (idle >= *sec_idle)
			continue;
		// HERE: least idle user (so far)
		*sec_idle = idle;
		*least_idle = u->user;
	}
	endutxent();

	if (is_udb_init == 0)
		goto done;

	// Check which user has awoken (from IDLE to NOT IDLE)
	GS_LIST_ITEM *li = NULL;
	while (1)
	{
		li = GS_LIST_next(&udb, li);
		if (li == NULL)
			break;

		struct utmp_db_user *u = (struct utmp_db_user *)li->data;
		if ((u->idle_old >= IDLE_THRESHOLD) && (u->idle < u->idle_old))
		{
			DEBUGF_R("Turned from IDLE to ACTIVE (awoken) (was %d, now %d)\n", u->idle_old, u->idle);
			GS_LIST_add(new_active, NULL, u->user, 0);
		}
		u->idle_old = u->idle;
	}

done:
	is_udb_init = 1;
}

