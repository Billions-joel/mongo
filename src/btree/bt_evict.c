/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __evict_dup_remove(WT_SESSION_IMPL *);
static int  __evict_file(WT_SESSION_IMPL *, WT_EVICT_REQ *);
static int  __evict_lru(WT_SESSION_IMPL *);
static int  __evict_lru_cmp(const void *, const void *);
static void __evict_pages(WT_SESSION_IMPL *);
static int  __evict_page_cmp(const void *, const void *);
static int  __evict_request_walk(WT_SESSION_IMPL *);
static int  __evict_walk(WT_SESSION_IMPL *);
static int  __evict_walk_file(WT_SESSION_IMPL *, u_int *);
static int  __evict_worker(WT_SESSION_IMPL *);

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some
 * number of pages from each file's in-memory tree for each page we evict.
 */
#define	WT_EVICT_GROUP		10	/* Evict N pages at a time */
#define	WT_EVICT_WALK_PER_TABLE	20	/* Pages to visit per file */
#define	WT_EVICT_WALK_BASE	100	/* Pages tracked across file visits */

/*
 * WT_EVICT_REQ_FOREACH --
 *	Walk a list of eviction requests.
 */
#define	WT_EVICT_REQ_FOREACH(er, er_end, cache)				\
	for ((er) = (cache)->evict_request,				\
	    (er_end) = (er) + (cache)->max_evict_request;		\
	    (er) < (er_end); ++(er))

/*
 * __evict_clr --
 *	Clear an entry in the eviction list.
 */
static inline void
__evict_clr(WT_EVICT_LIST *e)
{
	e->page = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __evict_req_set --
 *	Set an entry in the eviction request list.
 */
static inline void
__evict_req_set(
    WT_SESSION_IMPL *session, WT_EVICT_REQ *r, WT_PAGE *page, uint32_t flags)
{
					/* Should be empty */
	WT_ASSERT(session, r->session == NULL);

	WT_CLEAR(*r);
	r->btree = session->btree;
	r->page = page;
	r->flags = flags;

	/*
	 * Publish: there must be a barrier to ensure the structure fields are
	 * set before the eviction thread can see the request.
	 */
	WT_PUBLISH(r->session, session);
}

/*
 * __evict_req_clr --
 *	Clear an entry in the eviction request list.
 */
static inline void
__evict_req_clr(WT_SESSION_IMPL *session, WT_EVICT_REQ *r)
{
	WT_UNUSED(session);

	/*
	 * Publish; there must be a barrier to ensure the structure fields are
	 * set before the entry is made available for re-use.
	 */
	WT_PUBLISH(r->session, NULL);
}

/*
 * __wt_evict_server_wake --
 *	Wake the eviction server thread.
 */
void
__wt_evict_server_wake(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max;

	conn = S2C(session);
	cache = conn->cache;
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = conn->cache_size;

	WT_VERBOSE(session, evictserver,
	    "waking, bytes inuse %s max (%" PRIu64 "MB %s %" PRIu64 "MB), ",
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_inuse / WT_MEGABYTE,
	    bytes_inuse <= bytes_max ? "<=" : ">",
	    bytes_max / WT_MEGABYTE);

	__wt_cond_signal(session, cache->evict_cond);
}

/*
 * __evict_file_serial_func --
 *	Eviction serialization function called when a tree is being flushed
 *	or closed.
 */
void
__wt_evict_file_serial_func(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int close_method;

	__wt_evict_file_unpack(session, &close_method);

	cache = S2C(session)->cache;

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->session == NULL) {
			__evict_req_set(session, er, NULL, close_method ?
			    WT_EVICT_REQ_CLOSE : 0);
			return;
		}

	__wt_errx(session, "eviction server request table full");
	__wt_session_serialize_wrapup(session, NULL, WT_ERROR);
}

/*
 * __wt_evict_page_request --
 *	Schedule a page for forced eviction due to a high volume of inserts or
 *	updates.
 *
 *	NOTE: this function is called from inside serialized functions, so it
 *	is holding the serial lock.
 */
int
__wt_evict_page_request(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int owned;

	cache = S2C(session)->cache;

	/*
	 * Application threads request forced eviction of pages when they
	 * become too big.  The application thread must hold a hazard reference
	 * when this function is called, which protects it from being freed.
	 *
	 * However, it is possible (but unlikely) that the page is already part
	 * way through the process of being evicted: a thread may have selected
	 * it from the LRU list but not yet checked its hazard references.
	 *
	 * To avoid a freed page pointer ending up on the request list, we
	 * check the page state here while holding the LRU lock.  Since the
	 * state of page references in the eviction list is switched to
	 * WT_REF_EVICTING while holding the LRU lock, this check prevents a
	 * page from being evicted twice.
	 */
	owned = 0;
	__wt_spin_lock(session, &cache->lru_lock);
	if (!F_ISSET(page, WT_PAGE_FORCE_EVICT) &&
	    WT_ATOMIC_CAS(page->ref->state, WT_REF_MEM, WT_REF_EVICTING)) {
		F_SET(page, WT_PAGE_FORCE_EVICT);
		owned = 1;
	}
	__wt_spin_unlock(session, &cache->lru_lock);

	/*
	 * If we didn't swap the page state, some other thread is already
	 * evicting it, which is fine.
	 */
	if (!owned)
		return (0);

	/* Find an empty slot and enter the eviction request. */
	WT_EVICT_REQ_FOREACH(er, er_end, cache)
		if (er->session == NULL) {
			__evict_req_set(session, er, page, WT_EVICT_REQ_PAGE);
			__wt_evict_server_wake(session);
			return (0);
		}

	__wt_errx(session, "eviction server request table full");
	return (WT_ERROR);
}

/*
 * __wt_cache_evict_server --
 *	Thread to evict pages from the cache.
 */
void *
__wt_cache_evict_server(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *session;
	WT_CACHE *cache;
	int ret;

	conn = arg;
	cache = conn->cache;
	ret = 0;

	/*
	 * We need a session handle because we're reading/writing pages.
	 * Start with the default session to keep error handling simple.
	 */
	session = &conn->default_session;
	WT_ERR(__wt_open_session(conn, 1, NULL, NULL, &session));

	while (F_ISSET(conn, WT_SERVER_RUN)) {
		WT_VERBOSE(session, evictserver, "sleeping");
		__wt_cond_wait(session, cache->evict_cond);
		if (!F_ISSET(conn, WT_SERVER_RUN))
			break;
		WT_VERBOSE(session, evictserver, "waking");

		/* Evict pages from the cache as needed. */
		WT_ERR(__evict_worker(session));
	}

	if (ret == 0) {
		if (__wt_cache_bytes_inuse(cache) != 0) {
			__wt_errx(session,
			    "cache server: exiting with %" PRIu64 " pages, "
			    "%" PRIu64 " bytes in use",
			    __wt_cache_pages_inuse(cache),
			    __wt_cache_bytes_inuse(cache));
		}
	} else
err:		__wt_err(session, ret, "eviction server error");

	WT_VERBOSE(session, evictserver, "exiting");

	__wt_free(session, cache->evict);

	if (session != &conn->default_session)
		(void)session->iface.close(&session->iface, NULL);

	return (NULL);
}

/*
 * __evict_worker --
 *	Evict pages from memory.
 */
static int
__evict_worker(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_start, bytes_inuse, bytes_max;
	int loop;

	conn = S2C(session);
	cache = conn->cache;

	/* Evict pages from the cache. */
	for (loop = 0;; loop++) {
		/* Walk the eviction-request queue. */
		WT_RET(__evict_request_walk(session));

		/* Keep evicting until we hit 90% of the maximum cache size. */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;

		/*
		 * Keep evicting until we hit the target cache usage.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
		if (bytes_inuse < cache->eviction_target * (bytes_max / 100))
			break;

		WT_RET(__evict_lru(session));

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, go back to sleep, it's not something
		 * we can fix.
		 */
		bytes_start = bytes_inuse;
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		if (bytes_start == bytes_inuse) {
			if (loop == 10) {
				WT_STAT_INCR(conn->stats, cache_evict_slow);
				WT_VERBOSE(session, evictserver,
				    "unable to reach eviction goal");
				break;
			}
		} else
			loop = 0;
	}
	return (0);
}

/*
 * __evict_request_walk --
 *	Walk the eviction request queue.
 */
static int
__evict_request_walk(WT_SESSION_IMPL *session)
{
	WT_SESSION_IMPL *request_session;
	WT_CACHE *cache;
	WT_EVICT_REQ *er, *er_end;
	int ret;

	cache = S2C(session)->cache;

	/*
	 * Walk the eviction request queue, looking for sync or close requests
	 * (defined by a valid WT_SESSION_IMPL handle).  If we find a request,
	 * perform it, flush the result and clear the request slot, then wake
	 * up the requesting thread.
	 */
	WT_EVICT_REQ_FOREACH(er, er_end, cache) {
		if ((request_session = er->session) == NULL)
			continue;

		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, er->btree);

		/*
		 * Block out concurrent eviction while we are handling this
		 * request.
		 */
		__wt_spin_lock(session, &cache->lru_lock);

		/*
		 * The eviction candidate list might reference pages we are
		 * about to discard; clear it.
		 */
		memset(cache->evict, 0, cache->evict_allocated);

		/*
		 * Discard any page we're holding: we're about to do a walk of
		 * the file tree, and if we're closing the file, there won't be
		 * pages to evict in the future, that is, our location in the
		 * tree is no longer useful.
		 */
		session->btree->evict_page = NULL;

		if (F_ISSET(er, WT_EVICT_REQ_PAGE)) {
			WT_VERBOSE(session, evictserver,
			    "forcing eviction of page %p", er->page);
			for (;;) {
				ret = __wt_rec_evict(session, er->page, 0);
				if (ret != EBUSY)
					break;
				__wt_yield();
			}
		} else
			ret = __evict_file(session, er);

		/* Clear the reference to the btree handle. */
		WT_CLEAR_BTREE_IN_SESSION(session);

		__wt_spin_unlock(session, &cache->lru_lock);

		/*
		 * Resolve the request and clear the slot.
		 *
		 * !!!
		 * Page eviction is special: the requesting thread is already
		 * inside wrapup.
		 */
		if (!F_ISSET(er, WT_EVICT_REQ_PAGE))
			__wt_session_serialize_wrapup(
			    request_session, NULL, ret);

		__evict_req_clr(session, er);
	}
	return (0);
}

/*
 * __evict_file --
 *	Flush pages for a specific file as part of a close/sync operation.
 */
static int
__evict_file(WT_SESSION_IMPL *session, WT_EVICT_REQ *er)
{
	WT_PAGE *next_page, *page;

	WT_VERBOSE(session, evictserver,
	    "file request: %s",
	   (F_ISSET(er, WT_EVICT_REQ_CLOSE) ? "close" : "sync"));

	/*
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	next_page = NULL;
	WT_RET(__wt_tree_np(session, &next_page, 1, 1));
	for (;;) {
		if ((page = next_page) == NULL)
			break;
		WT_RET(__wt_tree_np(session, &next_page, 1, 1));

		/*
		 * Close: discarding all of the file's pages from the cache.
		 *  Sync: only dirty pages need to be written.
		 */
		if (F_ISSET(er, WT_EVICT_REQ_CLOSE))
			WT_RET(__wt_rec_evict(session, page, WT_REC_SINGLE));
		else if (__wt_page_is_modified(page))
			WT_RET(__wt_rec_write(session, page, NULL));
	}

	return (0);
}

/*
 * __evict_lru --
 *	Evict pages from the cache based on their read generation.
 */
static int
__evict_lru(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	int ret;

	cache = S2C(session)->cache;

	__wt_spin_lock(session, &cache->lru_lock);

	/* Get some more pages to consider for eviction. */
	WT_ERR(__evict_walk(session));

	/* Remove duplicates from the list. */
	__evict_dup_remove(session);

err:	__wt_spin_unlock(session, &cache->lru_lock);

	/* Reconcile and discard some pages. */
	if (ret == 0)
		__evict_pages(session);

	return (ret);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_BTREE *btree;
	WT_CACHE *cache;
	u_int elem, i;
	int ret;

	conn = S2C(session);
	cache = S2C(session)->cache;

	/*
	 * Resize the array in which we're tracking pages, as necessary, then
	 * get some pages from each underlying file.  We hold a mutex for the
	 * entire time -- it's slow, but (1) how often do new files get added
	 * or removed to/from the system, and (2) it's all in-memory stuff, so
	 * it's not that slow.
	 */
	ret = 0;
	__wt_spin_lock(session, &conn->spinlock);

	elem = WT_EVICT_WALK_BASE + (conn->btqcnt * WT_EVICT_WALK_PER_TABLE);
	if (elem > cache->evict_entries) {
		WT_ERR(__wt_realloc(session, &cache->evict_allocated,
		    elem * sizeof(WT_EVICT_LIST), &cache->evict));
		cache->evict_entries = elem;
	}
	cache->evict_current = cache->evict;

	i = WT_EVICT_WALK_BASE;
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		/* Reference the correct WT_BTREE handle. */
		WT_SET_BTREE_IN_SESSION(session, btree);

		ret = __evict_walk_file(session, &i);

		WT_CLEAR_BTREE_IN_SESSION(session);

		if (ret != 0)
			goto err;
	}

err:	__wt_spin_unlock(session, &conn->spinlock);
	return (ret);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(WT_SESSION_IMPL *session, u_int *slotp)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_PAGE *page;
	int i, restarted_once;

	btree = session->btree;
	cache = S2C(session)->cache;

	/*
	 * Get the next WT_EVICT_WALK_PER_TABLE entries.
	 *
	 * We can't evict the page just returned to us, it marks our place in
	 * the tree.  So, always stay one page ahead of the page being returned.
	 */
	i = restarted_once = 0;
	do {
		if ((page = btree->evict_page) == NULL)
			goto skip;

		/*
		 * Root and pinned pages can't be evicted.
		 * !!!
		 * It's still in flux if root pages are pinned or not, test for
		 * both cases for now.
		 */
		if (WT_PAGE_IS_ROOT(page) || F_ISSET(page, WT_PAGE_PINNED))
			goto skip;

		/*
		 * Skip locked pages: we would skip them later, and they just
		 * fill up the eviction list for no benefit.
		 */
		if (page->ref->state != WT_REF_MEM)
			goto skip;

		/*
		 * Skip pages expected to be merged into their parents.  The
		 * problem is if a parent and its child are both added to the
		 * eviction list and the child is merged into the parent when
		 * the parent is evicted, the child is left corrupted on the
		 * list (and might have already been selected for eviction by
		 * another thread).
		 */
		if (F_ISSET(page, WT_PAGE_REC_EMPTY |
		    WT_PAGE_REC_SPLIT | WT_PAGE_REC_SPLIT_MERGE))
			goto skip;

		WT_VERBOSE(session, evictserver,
		    "select: %p, size %" PRIu32, page, page->memory_footprint);

		++i;
		cache->evict[*slotp].page = page;
		cache->evict[*slotp].btree = btree;
		++*slotp;

skip:		WT_RET(__wt_tree_np(session, &btree->evict_page, 1, 1));
		if (btree->evict_page == NULL && restarted_once++ == 1)
			break;
	} while (i < WT_EVICT_WALK_PER_TABLE);

	return (0);
}

/*
 * __evict_dup_remove --
 *	Discard duplicates from the list of pages we collected.
 */
static void
__evict_dup_remove(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	u_int elem, i, j;

	cache = S2C(session)->cache;

	/*
	 * We have an array of page eviction references that may contain NULLs,
	 * as well as duplicate entries.
	 *
	 * First, sort the array by WT_REF address, then delete any duplicates.
	 * The reason is because we might evict the page but leave a duplicate
	 * entry in the "saved" area of the array, and that would be a NULL
	 * dereference on the next run.  (If someone ever tries to remove this
	 * duplicate cleanup for better performance, you can't fix it just by
	 * checking the WT_REF state -- that only works if you are discarding
	 * a page from a single level of the tree; if you are discarding a
	 * page and its parent, the duplicate of the page's WT_REF might have
	 * been free'd before a subsequent review of the eviction array.)
	 */
	evict = cache->evict;
	elem = cache->evict_entries;
	qsort(evict, (size_t)elem, sizeof(WT_EVICT_LIST), __evict_page_cmp);
	for (i = 0; i < elem; i = j) {
		/*
		 * Once we hit a NULL, we're done, the NULLs all sorted to the
		 * end of the array.
		 */
		if (evict[i].page == NULL)
			break;

		for (j = i + 1; j < elem; ++j) {
			/* Delete the second and any subsequent duplicates. */
			if (evict[i].page == evict[j].page)
				__evict_clr(&evict[j]);
			else
				break;
		}
	}

	/* Sort the array by LRU, then evict the most promising candidates. */
	qsort(cache->evict, elem, sizeof(WT_EVICT_LIST), __evict_lru_cmp);
}

/*
 * __evict_get_page --
 *	Get a page for eviction.
 */
static void
__evict_get_page(
    WT_SESSION_IMPL *session, int is_app, WT_BTREE **btreep, WT_PAGE **pagep)
{
	WT_CACHE *cache;
	WT_EVICT_LIST *evict;
	WT_REF *ref;

	cache = S2C(session)->cache;
	*btreep = NULL;
	*pagep = NULL;

	if (__wt_spin_trylock(session, &cache->lru_lock) != 0)
		return;

	evict = cache->evict_current;
	if (evict != NULL &&
	    evict >= cache->evict && evict < cache->evict + WT_EVICT_GROUP &&
	    evict->page != NULL) {
		WT_ASSERT(session, evict->btree != NULL);

		/*
		 * For now, application sessions can only evict pages from
		 * trees they have open.  Otherwise, closing a different
		 * session handle could cause the tree we are evicting from
		 * to be closed underneath us.
		 */
		if (is_app &&
		    __wt_session_has_btree(session, evict->btree) != 0)
			goto done;

		/* Move to the next page queued for eviction. */
		++cache->evict_current;

		/*
		 * If the page happens to be marked for forced eviction, ignore
		 * it: it will be sitting in the request queue.  This is
		 * unlikely, and it is simpler to leave it for the eviction
		 * thread than trying to find it and clear the request.
		 */
		if (F_ISSET(evict->page, WT_PAGE_FORCE_EVICT))
			goto done;

		/*
		 * Set the page locked here while holding the eviction mutex to
		 * prevent multiple attempts to evict it.
		 */
		ref = evict->page->ref;
		if (!WT_ATOMIC_CAS(ref->state, WT_REF_MEM, WT_REF_EVICTING))
			goto done;

		*pagep = evict->page;
		*btreep = evict->btree;

		/*
		 * If we're evicting our current eviction point in the file,
		 * clear it and restart the walk.
		 */
		if (*pagep == evict->btree->evict_page)
			evict->btree->evict_page = NULL;

		/*
		 * Paranoia: remove the entry so we never try and reconcile
		 * the same page on reconciliation error.
		 */
		__evict_clr(evict);
	}

done:	__wt_spin_unlock(session, &cache->lru_lock);
}

/*
 * __wt_evict_lru_page --
 *	Called by both eviction and application threads to evict a page.
 */
int
__wt_evict_lru_page(WT_SESSION_IMPL *session, int is_app)
{
	WT_BTREE *btree, *saved_btree;
	WT_PAGE *page;

	__evict_get_page(session, is_app, &btree, &page);
	if (page == NULL)
		return (WT_NOTFOUND);

	/* Reference the correct WT_BTREE handle. */
	saved_btree = session->btree;
	WT_SET_BTREE_IN_SESSION(session, btree);

	/*
	 * We don't care why eviction failed (maybe the page was dirty and we're
	 * out of disk space, or the page had an in-memory subtree already being
	 * evicted).  Regardless, don't pick the same page every time.
	 */
	if (__wt_rec_evict(session, page, 0) != 0) {
		page->read_gen = __wt_cache_read_gen(session);

		/*
		 * If the evicting state of the page was not cleared, clear it
		 * now to make the page available again.
		 */
		if (page->ref->state == WT_REF_EVICTING)
			page->ref->state = WT_REF_MEM;
	}

	WT_CLEAR_BTREE_IN_SESSION(session);
	session->btree = saved_btree;

	return (0);
}

/*
 * __evict_page --
 *	Reconcile and discard cache pages.
 */
static void
__evict_pages(WT_SESSION_IMPL *session)
{
	u_int i;

	for (i = 0; i < WT_EVICT_GROUP; i++)
		if (__wt_evict_lru_page(session, 0) != 0)
			break;
}

/*
 * __evict_page_cmp --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's address.
 */
static int
__evict_page_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;

	/*
	 * There may be NULL references in the array; sort them as greater than
	 * anything else so they migrate to the end of the array.
	 */
	a_page = ((WT_EVICT_LIST *)a)->page;
	b_page = ((WT_EVICT_LIST *)b)->page;
	if (a_page == NULL)
		return (b_page == NULL ? 0 : 1);
	if (b_page == NULL)
		return (-1);

	/* Sort the page address in ascending order. */
	return (a_page > b_page ? 1 : (a_page < b_page ? -1 : 0));
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort WT_EVICT_LIST array based on the page's read
 *	generation.
 */
static int
__evict_lru_cmp(const void *a, const void *b)
{
	WT_PAGE *a_page, *b_page;
	uint64_t a_lru, b_lru;

	/*
	 * There may be NULL references in the array; sort them as greater than
	 * anything else so they migrate to the end of the array.
	 */
	a_page = ((WT_EVICT_LIST *)a)->page;
	b_page = ((WT_EVICT_LIST *)b)->page;
	if (a_page == NULL)
		return (b_page == NULL ? 0 : 1);
	if (b_page == NULL)
		return (-1);

	/* Sort the LRU in ascending order. */
	a_lru = a_page->read_gen;
	b_lru = b_page->read_gen;

	/*
	 * Bias in favor of leaf pages.  Otherwise, we can waste time
	 * considering parent pages for eviction while their child pages are
	 * still in memory.
	 *
	 * Bump the LRU generation by a small fixed amount: the idea being that
	 * if we have enough good leaf page candidates, we should evict them
	 * first, but not completely ignore an old internal page.
	 */
	if (a_page->type == WT_PAGE_ROW_INT || a_page->type == WT_PAGE_COL_INT)
		a_lru += WT_EVICT_GROUP;
	if (b_page->type == WT_PAGE_ROW_INT || b_page->type == WT_PAGE_COL_INT)
		b_lru += WT_EVICT_GROUP;
	return (a_lru > b_lru ? 1 : (a_lru < b_lru ? -1 : 0));
}
