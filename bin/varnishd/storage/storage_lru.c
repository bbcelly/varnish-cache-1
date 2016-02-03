/*-
 * Copyright (c) 2007-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Dag-Erling Smørgav <des@des.no>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * STEVEDORE: one who works at or is responsible for loading and
 * unloading ships in port.  Example: "on the wharves, stevedores were
 * unloading cargo from the far corners of the world." Origin: Spanish
 * estibador, from estibar to pack.  First Known Use: 1788
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache/cache.h"
#include "hash/hash_slinger.h"

#include "storage/storage.h"
#include "vtim.h"

struct lru {
	unsigned		magic;
#define LRU_MAGIC		0x3fec7bb0
	VTAILQ_HEAD(,objcore)	lru_head;
	struct lock		mtx;
};

struct lru *
LRU_Alloc(void)
{
	struct lru *l;

	ALLOC_OBJ(l, LRU_MAGIC);
	AN(l);
	VTAILQ_INIT(&l->lru_head);
	Lck_New(&l->mtx, lck_lru);
	return (l);
}

void
LRU_Free(struct lru *lru)
{
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Delete(&lru->mtx);
	FREE_OBJ(lru);
}

void
LRU_Add(struct objcore *oc)
{
	struct lru *lru;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	lru = ObjGetLRU(oc);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Lock(&lru->mtx);
	VTAILQ_INSERT_TAIL(&lru->lru_head, oc, lru_list);
	oc->last_lru = VTIM_real();
	Lck_Unlock(&lru->mtx);
}

void
LRU_Remove(struct objcore *oc)
{
	struct lru *lru;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	lru = ObjGetLRU(oc);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	Lck_Lock(&lru->mtx);
	if (!isnan(oc->last_lru)) {
		VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);
		oc->last_lru = NAN;
	}
	Lck_Unlock(&lru->mtx);
}

void __match_proto__(objtouch_f)
LRU_Touch(struct worker *wrk, struct objcore *oc, double now)
{
	struct lru *lru;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

	/*
	 * To avoid the exphdl->mtx becoming a hotspot, we only
	 * attempt to move objects if they have not been moved
	 * recently and if the lock is available.  This optimization
	 * obviously leaves the LRU list imperfectly sorted.
	 */

	if (oc->flags & OC_F_INCOMPLETE)
		return;

	if (now - oc->last_lru < cache_param->lru_interval)
		return;

	lru = ObjGetLRU(oc);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);

	if (Lck_Trylock(&lru->mtx))
		return;

	if (!isnan(oc->last_lru)) {
		/* Can only touch it while it's actually on the LRU list */
		VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);
		VTAILQ_INSERT_TAIL(&lru->lru_head, oc, lru_list);
		VSC_C_main->n_lru_moved++;
		oc->last_lru = now;
	}
	Lck_Unlock(&lru->mtx);
}

/*--------------------------------------------------------------------
 * Attempt to make space by nuking the oldest object on the LRU list
 * which isn't in use.
 * Returns: 1: did, 0: didn't, -1: can't
 */

int
LRU_NukeOne(struct worker *wrk, struct lru *lru)
{
	struct objcore *oc, *oc2;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(lru, LRU_MAGIC);
	/* Find the first currently unused object on the LRU.  */
	Lck_Lock(&lru->mtx);
	VTAILQ_FOREACH_SAFE(oc, &lru->lru_head, lru_list, oc2) {
		CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

		VSLb(wrk->vsl, SLT_ExpKill, "LRU_Cand p=%p f=0x%x r=%d",
		    oc, oc->flags, oc->refcnt);

		AZ(isnan(oc->last_lru));

		if (ObjSnipe(wrk, oc)) {
			VSC_C_main->n_lru_nuked++; // XXX per lru ?
			VTAILQ_REMOVE(&lru->lru_head, oc, lru_list);
			oc->last_lru = NAN;
			break;
		}
	}
	Lck_Unlock(&lru->mtx);

	if (oc == NULL) {
		VSLb(wrk->vsl, SLT_ExpKill, "LRU_Fail");
		return (-1);
	}

	/* XXX: We could grab and return one storage segment to our caller */
	ObjSlim(wrk, oc);

	EXP_Poke(oc);

	VSLb(wrk->vsl, SLT_ExpKill, "LRU x=%u", ObjGetXID(wrk, oc));
	(void)HSH_DerefObjCore(wrk, &oc);
	return (1);
}
