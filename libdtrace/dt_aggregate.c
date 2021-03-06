/*
 * Oracle Linux DTrace.
 * Copyright (c) 2008, 2020, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dt_impl.h>
#include <dtrace.h>
#include <assert.h>
#include <alloca.h>
#include <limits.h>

#include <libproc.h>
#include <port.h>
#include <dt_bpf.h>

#define	DTRACE_AHASHSIZE	32779		/* big 'ol prime */

/*
 * Because qsort(3C) does not allow an argument to be passed to a comparison
 * function, the variables that affect comparison must regrettably be global;
 * they are protected by a global static lock, dt_qsort_lock.
 */
static pthread_mutex_t dt_qsort_lock = PTHREAD_MUTEX_INITIALIZER;

static int dt_revsort;
static int dt_keysort;
static int dt_keypos;

#define	DT_LESSTHAN	(dt_revsort == 0 ? -1 : 1)
#define	DT_GREATERTHAN	(dt_revsort == 0 ? 1 : -1)

static int
dt_aggregate_countcmp(int64_t *lhs, int64_t *rhs)
{
	int64_t lvar = *lhs;
	int64_t rvar = *rhs;

	if (lvar < rvar)
		return DT_LESSTHAN;

	if (lvar > rvar)
		return DT_GREATERTHAN;

	return 0;
}

static int
dt_aggregate_averagecmp(int64_t *lhs, int64_t *rhs)
{
	int64_t lavg = lhs[0] ? (lhs[1] / lhs[0]) : 0;
	int64_t ravg = rhs[0] ? (rhs[1] / rhs[0]) : 0;

	if (lavg < ravg)
		return DT_LESSTHAN;

	if (lavg > ravg)
		return DT_GREATERTHAN;

	return 0;
}

static int
dt_aggregate_stddevcmp(int64_t *lhs, int64_t *rhs)
{
	uint64_t lsd = dt_stddev((uint64_t *)lhs, 1);
	uint64_t rsd = dt_stddev((uint64_t *)rhs, 1);

	if (lsd < rsd)
		return DT_LESSTHAN;

	if (lsd > rsd)
		return DT_GREATERTHAN;

	return 0;
}

static long double
dt_aggregate_lquantizedsum(int64_t *lquanta)
{
	int64_t arg = *lquanta++;
	int32_t base = DTRACE_LQUANTIZE_BASE(arg);
	uint16_t step = DTRACE_LQUANTIZE_STEP(arg);
	uint16_t levels = DTRACE_LQUANTIZE_LEVELS(arg), i;
	long double total = (long double)lquanta[0] * (long double)(base - 1);

	for (i = 0; i < levels; base += step, i++)
		total += (long double)lquanta[i + 1] * (long double)base;

	return total +
	       (long double)lquanta[levels + 1] * (long double)(base + 1);
}

static int64_t
dt_aggregate_lquantizedzero(int64_t *lquanta)
{
	int64_t arg = *lquanta++;
	int32_t base = DTRACE_LQUANTIZE_BASE(arg);
	uint16_t step = DTRACE_LQUANTIZE_STEP(arg);
	uint16_t levels = DTRACE_LQUANTIZE_LEVELS(arg), i;

	if (base - 1 == 0)
		return lquanta[0];

	for (i = 0; i < levels; base += step, i++) {
		if (base != 0)
			continue;

		return lquanta[i + 1];
	}

	if (base + 1 == 0)
		return lquanta[levels + 1];

	return 0;
}

static int
dt_aggregate_lquantizedcmp(int64_t *lhs, int64_t *rhs)
{
	long double lsum = dt_aggregate_lquantizedsum(lhs);
	long double rsum = dt_aggregate_lquantizedsum(rhs);
	int64_t lzero, rzero;

	if (lsum < rsum)
		return DT_LESSTHAN;

	if (lsum > rsum)
		return DT_GREATERTHAN;

	/*
	 * If they're both equal, then we will compare based on the weights at
	 * zero.  If the weights at zero are equal (or if zero is not within
	 * the range of the linear quantization), then this will be judged a
	 * tie and will be resolved based on the key comparison.
	 */
	lzero = dt_aggregate_lquantizedzero(lhs);
	rzero = dt_aggregate_lquantizedzero(rhs);

	if (lzero < rzero)
		return DT_LESSTHAN;

	if (lzero > rzero)
		return DT_GREATERTHAN;

	return 0;
}

/* called by dt_aggregate_llquantizedcmp() */
static long double
dt_aggregate_llquantizedsum(int64_t *llquanta)
{
	int64_t arg = *llquanta++;
	int factor = DTRACE_LLQUANTIZE_FACTOR(arg);
	int lmag = DTRACE_LLQUANTIZE_LMAG(arg);
	int hmag = DTRACE_LLQUANTIZE_HMAG(arg);
	int steps = DTRACE_LLQUANTIZE_STEPS(arg);
	int steps_factor = steps / factor;

	int bin0 = 1 + (hmag-lmag+1) * (steps-steps_factor);

	long double total = powl(factor, lmag) *
	    (llquanta[bin0+1]-llquanta[bin0-1]);
	long double scale;
	int step, mag, i;

	i = 1;
	if (lmag==0 && steps > factor) {
		for (step = 2; step <= factor; step++) {
			i += steps_factor;
			total += step * (llquanta[bin0+i] - llquanta[bin0-i]);
		}
		lmag = 1;
	}
	scale = powl(factor, lmag+1) / steps;
	for (mag = lmag; mag <= hmag; mag++) {
		for (step = steps_factor + 1; step <= steps; step++) {
			i++;
			total += step * scale *
			    (llquanta[bin0+i] - llquanta[bin0-i]);
		}
		scale *= factor;
	}

	return total;
}

/* called by dt_aggregate_llquantizedcmp() */
static int64_t
dt_aggregate_llquantizedzero(int64_t *llquanta)
{
	int64_t arg = *llquanta++;
	uint16_t factor = DTRACE_LLQUANTIZE_FACTOR(arg);
	uint16_t lmag = DTRACE_LLQUANTIZE_LMAG(arg);
	uint16_t hmag = DTRACE_LLQUANTIZE_HMAG(arg);
	uint16_t steps = DTRACE_LLQUANTIZE_STEPS(arg);
	uint16_t underflow_bin = 1 + (hmag-lmag+1) * (steps-steps/factor);

	/*
	 * We'll define "zero" here as being the underflow bin.
	 */
	return llquanta[underflow_bin];
}

/*
 * For sorting aggregations for printing.
 * Detailed behavior is not documented,
 * but merely inherited from Solaris.
 * Other behavior is also reasonable.
 */
static int
dt_aggregate_llquantizedcmp(int64_t *lhs, int64_t *rhs)
{
	long double lsum = dt_aggregate_llquantizedsum(lhs);
	long double rsum = dt_aggregate_llquantizedsum(rhs);
	int64_t lzero, rzero;

	if (lsum < rsum)
		return DT_LESSTHAN;

	if (lsum > rsum)
		return DT_GREATERTHAN;

	/*
	 * If they're both equal, then we will compare based on the weights at
	 * zero.  If the weights at zero are equal (or if zero is not within
	 * the range of the linear quantization), then this will be judged a
	 * tie and will be resolved based on the key comparison.
	 */
	lzero = dt_aggregate_llquantizedzero(lhs);
	rzero = dt_aggregate_llquantizedzero(rhs);

	if (lzero < rzero)
		return DT_LESSTHAN;

	if (lzero > rzero)
		return DT_GREATERTHAN;

	return 0;
}

static int
dt_aggregate_quantizedcmp(int64_t *lhs, int64_t *rhs)
{
	int nbuckets = DTRACE_QUANTIZE_NBUCKETS, i;
	long double ltotal = 0, rtotal = 0;
	int64_t lzero = 0, rzero = 0;

	for (i = 0; i < nbuckets; i++) {
		int64_t bucketval = DTRACE_QUANTIZE_BUCKETVAL(i);

		if (bucketval == 0) {
			lzero = lhs[i];
			rzero = rhs[i];
		}

		ltotal += (long double)bucketval * (long double)lhs[i];
		rtotal += (long double)bucketval * (long double)rhs[i];
	}

	if (ltotal < rtotal)
		return DT_LESSTHAN;

	if (ltotal > rtotal)
		return DT_GREATERTHAN;

	/*
	 * If they're both equal, then we will compare based on the weights at
	 * zero.  If the weights at zero are equal, then this will be judged a
	 * tie and will be resolved based on the key comparison.
	 */
	if (lzero < rzero)
		return DT_LESSTHAN;

	if (lzero > rzero)
		return DT_GREATERTHAN;

	return 0;
}

static void
dt_aggregate_usym(dtrace_hdl_t *dtp, uint64_t *data)
{
	uint64_t tgid = data[1];
	uint64_t *pc = &data[2];
	pid_t pid;
	GElf_Sym sym;

	if (dtp->dt_vector != NULL)
		return;

	pid = dt_proc_grab_lock(dtp, tgid, DTRACE_PROC_WAITING |
	    DTRACE_PROC_SHORTLIVED);
	if (pid < 0)
		return;

	if (dt_Plookup_by_addr(dtp, pid, *pc, NULL, &sym) == 0)
		*pc = sym.st_value;

	dt_proc_release_unlock(dtp, pid);
}

static void
dt_aggregate_umod(dtrace_hdl_t *dtp, uint64_t *data)
{
	uint64_t tgid = data[1];
	uint64_t *pc = &data[2];
	pid_t pid;
	const prmap_t *map;

	if (dtp->dt_vector != NULL)
		return;

	pid = dt_proc_grab_lock(dtp, tgid, DTRACE_PROC_WAITING |
	    DTRACE_PROC_SHORTLIVED);
	if (pid < 0)
		return;

	if ((map = dt_Paddr_to_map(dtp, pid, *pc)) != NULL)
		*pc = map->pr_vaddr;

	dt_proc_release_unlock(dtp, pid);
}

static void
dt_aggregate_sym(dtrace_hdl_t *dtp, uint64_t *data)
{
	GElf_Sym sym;
	uint64_t *pc = data;

	if (dtrace_lookup_by_addr(dtp, *pc, &sym, NULL) == 0)
		*pc = sym.st_value;
}

static void
dt_aggregate_mod(dtrace_hdl_t *dtp, uint64_t *addr)
{
	dt_module_t *dmp;

	if (dtp->dt_vector != NULL) {
		/*
		 * We don't have a way of just getting the module for a
		 * vectored open, and it doesn't seem to be worth defining
		 * one.  This means that use of mod() won't get true
		 * aggregation in the postmortem case (some modules may
		 * appear more than once in aggregation output).  It seems
		 * unlikely that anyone will ever notice or care...
		 */
		return;
	}

	for (dmp = dt_list_next(&dtp->dt_modlist); dmp != NULL;
	    dmp = dt_list_next(dmp)) {
		/*
		 * Check text and data ranges for match.  If there is a range
		 * that covers the given address, normalize it.
		 */
		if (dmp->dm_text_addrs != NULL &&
		    bsearch(addr, dmp->dm_text_addrs, dmp->dm_text_addrs_size,
			    sizeof(struct dtrace_addr_range),
			    dtrace_addr_range_cmp) != NULL) {

			*addr = dmp->dm_text_addrs[0].dar_va;
			return;
		}

		if (dmp->dm_data_addrs != NULL &&
		    bsearch(addr, dmp->dm_data_addrs, dmp->dm_data_addrs_size,
			    sizeof(struct dtrace_addr_range),
			    dtrace_addr_range_cmp) != NULL) {

			if (dmp->dm_text_addrs != NULL)
				*addr = dmp->dm_text_addrs[0].dar_va;
			else
				*addr = dmp->dm_data_addrs[0].dar_va;

			return;
		}
	}
}

static dtrace_aggid_t
dt_aggregate_aggid(dt_ahashent_t *ent)
{
	dtrace_aggdesc_t *agg = ent->dtahe_data.dtada_desc;
	caddr_t data = ent->dtahe_data.dtada_data;
	dtrace_recdesc_t *rec = agg->dtagd_recs;

	/*
	 * First, we'll check the variable ID in the aggdesc.  If it's valid,
	 * we'll return it.  If not, we'll use the compiler-generated ID
	 * present as the first record.
	 */
	if (agg->dtagd_varid != DTRACE_AGGVARIDNONE)
		return agg->dtagd_varid;

	agg->dtagd_varid = *((dtrace_aggid_t *)
					(uintptr_t)(data + rec->dtrd_offset));

	return agg->dtagd_varid;
}

typedef struct dt_snapstate {
	dtrace_hdl_t	*dtp;
	processorid_t	cpu;
	char		*buf;
	dt_aggregate_t	*agp;
} dt_snapstate_t;

static int
dt_aggregate_snap_one(dt_idhash_t *dhp, dt_ident_t *aid, dt_snapstate_t *st)
{
	dt_ahash_t		*agh = &st->agp->dtat_hash;
	dt_ahashent_t		*h;
	dtrace_aggdesc_t	*agg;
	dtrace_aggdata_t	*agd;
	uint64_t		hval = aid->di_id;
	size_t			ndx = hval % agh->dtah_size;
	int			rval;
	uint_t			i;

	rval = dt_aggid_lookup(st->dtp, aid->di_id, &agg);
	if (rval != 0)
		return rval;

	/* See if we already have an entry for this aggregation. */
	for (h = agh->dtah_hash[ndx]; h != NULL; h = h->dtahe_next) {
		int64_t	*src, *dst, *cpu_dst;
		uint_t	cnt;

		if (h->dtahe_hval != hval)
			continue;
		if (h->dtahe_size != aid->di_size)
			continue;

		/* Entry found - process the data. */
		agd = &h->dtahe_data;
		dst = (int64_t *)agd->dtada_data;
		cpu_dst = agd->dtada_percpu != NULL
				? (int64_t *)agd->dtada_percpu[st->cpu]
				: NULL;

accumulate:
		src = (int64_t *)(st->buf + aid->di_offset);
		switch (((dt_ident_t *)aid->di_iarg)->di_id) {
		case DT_AGG_MAX:
			if (*src > *dst)
				*dst = *src;

			break;
		case DT_AGG_MIN:
			if (*src < *dst)
				*dst = *src;

			break;
		default:
			for (i = 0, cnt = aid->di_size / sizeof(int64_t);
			     i < cnt; i++, dst++, src++)
				*dst += *src;
		}

		/* If we keep per-CPU data - process that as well. */
		if (cpu_dst != NULL) {
			dst = cpu_dst;
			cpu_dst = NULL;

			goto accumulate;
		}

		return 0;
	}

	/* Not found - add it. */
	h = dt_zalloc(st->dtp, sizeof(dt_ahashent_t));
	if (h == NULL)
		return dt_set_errno(st->dtp, EDT_NOMEM);

	agd = &h->dtahe_data;
	agd->dtada_data = dt_alloc(st->dtp, aid->di_size);
	if (agd->dtada_data == NULL) {
		dt_free(st->dtp, h);
		return dt_set_errno(st->dtp, EDT_NOMEM);
	}

	memcpy(agd->dtada_data, st->buf + aid->di_offset, aid->di_size);
	agd->dtada_size = aid->di_size;
	agd->dtada_desc = agg;
	agd->dtada_hdl = st->dtp;
	agd->dtada_normal = 1;

	h->dtahe_hval = hval;
	h->dtahe_size = aid->di_size;

	if (st->agp->dtat_flags & DTRACE_A_PERCPU) {
		char	**percpu = dt_calloc(st->dtp,
					     st->dtp->dt_conf.max_cpuid + 1,
					     sizeof(char *));

		if (percpu == NULL) {
			dt_free(st->dtp, agd->dtada_data);
			dt_free(st->dtp, h);

			dt_set_errno(st->dtp, EDT_NOMEM);
		}

		for (i = 0; i <= st->dtp->dt_conf.max_cpuid; i++) {
			percpu[i] = dt_zalloc(st->dtp, aid->di_size);
			if (percpu[i] == NULL) {
				while (--i >= 0)
					dt_free(st->dtp, percpu[i]);
				dt_free(st->dtp, agd->dtada_data);
				dt_free(st->dtp, h);

				dt_set_errno(st->dtp, EDT_NOMEM);
			}
		}

		memcpy(percpu[st->cpu], st->buf + aid->di_offset, aid->di_size);
		agd->dtada_percpu = percpu;
	}

	/* Add the new entry to the hashtable. */
	if (agh->dtah_hash[ndx] != NULL)
		agh->dtah_hash[ndx]->dtahe_prev = h;

	h->dtahe_next = agh->dtah_hash[ndx];
	agh->dtah_hash[ndx] = h;

	if (agh->dtah_all != NULL)
		agh->dtah_all->dtahe_prevall = h;

	h->dtahe_nextall = agh->dtah_all;
	agh->dtah_all = h;

	return 0;
}

static int
dt_aggregate_snap_cpu(dtrace_hdl_t *dtp, processorid_t cpu)
{
	dt_aggregate_t	*agp = &dtp->dt_aggregate;
	char		*buf = agp->dtat_cpu_buf[cpu];
	uint64_t	*seq = (uint64_t *)buf;
	dt_snapstate_t	st;

	/* Nothing to be done? */
	if (*seq == 0)
		return 0;

	/*
	 * Process all static aggregation identifiers in the CPU buffer.  We
	 * skip over the latch sequence number because from this point on we
	 * are simply processing data.
	 */
	st.dtp = dtp;
	st.cpu = cpu;
	st.buf = buf + sizeof(*seq);
	st.agp = agp;

	return dt_idhash_iter(dtp->dt_aggs,
			      (dt_idhash_f *)dt_aggregate_snap_one, &st);
}

/*
 * Retrieve all aggregation data for the enabled CPUs and aggregate it.
 */
int
dtrace_aggregate_snap(dtrace_hdl_t *dtp)
{
	dt_aggregate_t	*agp = &dtp->dt_aggregate;
	uint32_t	key = 0;
	int		i, rval;

	/*
	 * If we do not have a buffer initialized, we will not be processing
	 * aggregations, so there is nothing to be done here.
	 */
	if (agp->dtat_cpu_buf == NULL)
		return 0;

	rval = dt_bpf_map_lookup(dtp->dt_aggmap_fd, &key, agp->dtat_buf);
	if (rval != 0)
		return dt_set_errno(dtp, -rval);

	for (i = 0; i < dtp->dt_conf.num_online_cpus; i++) {
		rval = dt_aggregate_snap_cpu(dtp, dtp->dt_conf.cpus[i].cpu_id);
		if (rval != 0)
			return rval;
	}

	return 0;
}

static int
dt_aggregate_hashcmp(const void *lhs, const void *rhs)
{
	dt_ahashent_t *lh = *((dt_ahashent_t **)lhs);
	dt_ahashent_t *rh = *((dt_ahashent_t **)rhs);
	dtrace_aggdesc_t *lagg = lh->dtahe_data.dtada_desc;
	dtrace_aggdesc_t *ragg = rh->dtahe_data.dtada_desc;

	if (lagg->dtagd_nrecs < ragg->dtagd_nrecs)
		return DT_LESSTHAN;

	if (lagg->dtagd_nrecs > ragg->dtagd_nrecs)
		return DT_GREATERTHAN;

	return 0;
}

static int
dt_aggregate_varcmp(const void *lhs, const void *rhs)
{
	dt_ahashent_t	*lh = *((dt_ahashent_t **)lhs);
	dt_ahashent_t	*rh = *((dt_ahashent_t **)rhs);
	dtrace_aggid_t	lid, rid;

	lid = dt_aggregate_aggid(lh);
	rid = dt_aggregate_aggid(rh);

	if (lid < rid)
		return DT_LESSTHAN;

	if (lid > rid)
		return DT_GREATERTHAN;

	return 0;
}

static int
dt_aggregate_keycmp(const void *lhs, const void *rhs)
{
	dt_ahashent_t *lh = *((dt_ahashent_t **)lhs);
	dt_ahashent_t *rh = *((dt_ahashent_t **)rhs);
	dtrace_aggdesc_t *lagg = lh->dtahe_data.dtada_desc;
	dtrace_aggdesc_t *ragg = rh->dtahe_data.dtada_desc;
	dtrace_recdesc_t *lrec, *rrec;
	char *ldata, *rdata;
	int rval, i, j, keypos, nrecs;

	if ((rval = dt_aggregate_hashcmp(lhs, rhs)) != 0)
		return rval;

	nrecs = lagg->dtagd_nrecs - 1;
	assert(nrecs == ragg->dtagd_nrecs - 1);

	keypos = dt_keypos + 1 >= nrecs ? 0 : dt_keypos;

	for (i = 1; i < nrecs; i++) {
		uint64_t lval, rval;
		int ndx = i + keypos;

		if (ndx >= nrecs)
			ndx = ndx - nrecs + 1;

		lrec = &lagg->dtagd_recs[ndx];
		rrec = &ragg->dtagd_recs[ndx];

		ldata = lh->dtahe_data.dtada_data + lrec->dtrd_offset;
		rdata = rh->dtahe_data.dtada_data + rrec->dtrd_offset;

		if (lrec->dtrd_size < rrec->dtrd_size)
			return DT_LESSTHAN;

		if (lrec->dtrd_size > rrec->dtrd_size)
			return DT_GREATERTHAN;

		switch (lrec->dtrd_size) {
		case sizeof(uint64_t):
			/* LINTED - alignment */
			lval = *((uint64_t *)ldata);
			/* LINTED - alignment */
			rval = *((uint64_t *)rdata);
			break;

		case sizeof(uint32_t):
			/* LINTED - alignment */
			lval = *((uint32_t *)ldata);
			/* LINTED - alignment */
			rval = *((uint32_t *)rdata);
			break;

		case sizeof(uint16_t):
			/* LINTED - alignment */
			lval = *((uint16_t *)ldata);
			/* LINTED - alignment */
			rval = *((uint16_t *)rdata);
			break;

		case sizeof(uint8_t):
			lval = *((uint8_t *)ldata);
			rval = *((uint8_t *)rdata);
			break;

		default:
			switch (lrec->dtrd_action) {
			case DTRACEACT_UMOD:
			case DTRACEACT_UADDR:
			case DTRACEACT_USYM:
				for (j = 0; j < 2; j++) {
					/* LINTED - alignment */
					lval = ((uint64_t *)ldata)[j];
					/* LINTED - alignment */
					rval = ((uint64_t *)rdata)[j];

					if (lval < rval)
						return DT_LESSTHAN;

					if (lval > rval)
						return DT_GREATERTHAN;
				}

				break;

			default:
				for (j = 0; j < lrec->dtrd_size; j++) {
					lval = ((uint8_t *)ldata)[j];
					rval = ((uint8_t *)rdata)[j];

					if (lval < rval)
						return DT_LESSTHAN;

					if (lval > rval)
						return DT_GREATERTHAN;
				}
			}

			continue;
		}

		if (lval < rval)
			return DT_LESSTHAN;

		if (lval > rval)
			return DT_GREATERTHAN;
	}

	return 0;
}

static int
dt_aggregate_valcmp(const void *lhs, const void *rhs)
{
	dt_ahashent_t *lh = *((dt_ahashent_t **)lhs);
	dt_ahashent_t *rh = *((dt_ahashent_t **)rhs);
	dtrace_aggdesc_t *lagg = lh->dtahe_data.dtada_desc;
	dtrace_aggdesc_t *ragg = rh->dtahe_data.dtada_desc;
	caddr_t ldata = lh->dtahe_data.dtada_data;
	caddr_t rdata = rh->dtahe_data.dtada_data;
	dtrace_recdesc_t *lrec, *rrec;
	int64_t *laddr, *raddr;
	int rval, i;

	if ((rval = dt_aggregate_hashcmp(lhs, rhs)) != 0)
		return rval;

	if (lagg->dtagd_nrecs > ragg->dtagd_nrecs)
		return DT_GREATERTHAN;

	if (lagg->dtagd_nrecs < ragg->dtagd_nrecs)
		return DT_LESSTHAN;

	if (lagg->dtagd_nrecs <= 0)
	    return 0;

	for (i = 0; i < lagg->dtagd_nrecs; i++) {
		lrec = &lagg->dtagd_recs[i];
		rrec = &ragg->dtagd_recs[i];

		if (lrec->dtrd_offset < rrec->dtrd_offset)
			return DT_LESSTHAN;

		if (lrec->dtrd_offset > rrec->dtrd_offset)
			return DT_GREATERTHAN;

		if (lrec->dtrd_action < rrec->dtrd_action)
			return DT_LESSTHAN;

		if (lrec->dtrd_action > rrec->dtrd_action)
			return DT_GREATERTHAN;
	}

	laddr = (int64_t *)(uintptr_t)(ldata + lrec->dtrd_offset);
	raddr = (int64_t *)(uintptr_t)(rdata + rrec->dtrd_offset);

	switch (lrec->dtrd_action) {
	case DTRACEAGG_AVG:
		rval = dt_aggregate_averagecmp(laddr, raddr);
		break;

	case DTRACEAGG_STDDEV:
		rval = dt_aggregate_stddevcmp(laddr, raddr);
		break;

	case DTRACEAGG_QUANTIZE:
		rval = dt_aggregate_quantizedcmp(laddr, raddr);
		break;

	case DTRACEAGG_LQUANTIZE:
		rval = dt_aggregate_lquantizedcmp(laddr, raddr);
		break;

	case DTRACEAGG_LLQUANTIZE:
		rval = dt_aggregate_llquantizedcmp(laddr, raddr);
		break;

	case DTRACEAGG_COUNT:
	case DTRACEAGG_SUM:
	case DTRACEAGG_MIN:
	case DTRACEAGG_MAX:
		rval = dt_aggregate_countcmp(laddr, raddr);
		break;

	default:
		assert(0);
	}

	return rval;
}

static int
dt_aggregate_valkeycmp(const void *lhs, const void *rhs)
{
	int rval;

	if ((rval = dt_aggregate_valcmp(lhs, rhs)) != 0)
		return rval;

	/*
	 * If we're here, the values for the two aggregation elements are
	 * equal.  We already know that the key layout is the same for the two
	 * elements; we must now compare the keys themselves as a tie-breaker.
	 */
	return dt_aggregate_keycmp(lhs, rhs);
}

static int
dt_aggregate_keyvarcmp(const void *lhs, const void *rhs)
{
	int rval;

	if ((rval = dt_aggregate_keycmp(lhs, rhs)) != 0)
		return rval;

	return dt_aggregate_varcmp(lhs, rhs);
}

static int
dt_aggregate_varkeycmp(const void *lhs, const void *rhs)
{
	int rval;

	if ((rval = dt_aggregate_varcmp(lhs, rhs)) != 0)
		return rval;

	return dt_aggregate_keycmp(lhs, rhs);
}

static int
dt_aggregate_valvarcmp(const void *lhs, const void *rhs)
{
	int rval;

	if ((rval = dt_aggregate_valkeycmp(lhs, rhs)) != 0)
		return rval;

	return dt_aggregate_varcmp(lhs, rhs);
}

static int
dt_aggregate_varvalcmp(const void *lhs, const void *rhs)
{
	int rval;

	if ((rval = dt_aggregate_varcmp(lhs, rhs)) != 0)
		return rval;

	return dt_aggregate_valkeycmp(lhs, rhs);
}

static int
dt_aggregate_keyvarrevcmp(const void *lhs, const void *rhs)
{
	return dt_aggregate_keyvarcmp(rhs, lhs);
}

static int
dt_aggregate_varkeyrevcmp(const void *lhs, const void *rhs)
{
	return dt_aggregate_varkeycmp(rhs, lhs);
}

static int
dt_aggregate_valvarrevcmp(const void *lhs, const void *rhs)
{
	return dt_aggregate_valvarcmp(rhs, lhs);
}

static int
dt_aggregate_varvalrevcmp(const void *lhs, const void *rhs)
{
	return dt_aggregate_varvalcmp(rhs, lhs);
}

static int
dt_aggregate_bundlecmp(const void *lhs, const void *rhs)
{
	dt_ahashent_t **lh = *((dt_ahashent_t ***)lhs);
	dt_ahashent_t **rh = *((dt_ahashent_t ***)rhs);
	int i, rval;

	if (dt_keysort) {
		/*
		 * If we're sorting on keys, we need to scan until we find the
		 * last entry -- that's the representative key.  (The order of
		 * the bundle is values followed by key to accommodate the
		 * default behavior of sorting by value.)  If the keys are
		 * equal, we'll fall into the value comparison loop, below.
		 */
		for (i = 0; lh[i + 1] != NULL; i++)
			continue;

		assert(i != 0);
		assert(rh[i + 1] == NULL);

		if ((rval = dt_aggregate_keycmp(&lh[i], &rh[i])) != 0)
			return rval;
	}

	for (i = 0; ; i++) {
		if (lh[i + 1] == NULL) {
			/*
			 * All of the values are equal; if we're sorting on
			 * keys, then we're only here because the keys were
			 * found to be equal and these records are therefore
			 * equal.  If we're not sorting on keys, we'll use the
			 * key comparison from the representative key as the
			 * tie-breaker.
			 */
			if (dt_keysort)
				return 0;

			assert(i != 0);
			assert(rh[i + 1] == NULL);
			return dt_aggregate_keycmp(&lh[i], &rh[i]);
		} else {
			if ((rval = dt_aggregate_valcmp(&lh[i], &rh[i])) != 0)
				return rval;
		}
	}
}

/*
 * Callback for initializing the min() and max() aggregation functions.
 * The minimum 64 bit value is set for max() and the maximum 64 bit value is
 * set for min(), so that any other value fed to the functions will register
 * properly.
 *
 * The first value in the buffer is used as a flag to indicate whether an
 * initial value was stored in the buffer.
 */
static int
init_minmax(dt_idhash_t *dhp, dt_ident_t *aid, char *buf)
{
	dt_ident_t *fid = aid->di_iarg;
	int64_t *ptr;
	int64_t value;

	assert(aid->di_kind == DT_IDENT_AGG);
	assert(fid);

	if (fid->di_id == DT_AGG_MIN)
		value = INT64_MAX;
	else if (fid->di_id == DT_AGG_MAX)
		value = INT64_MIN;
	else
		return 0;

	/* Indicate that we are setting initial values. */
	*(int64_t *)buf = 1;

	ptr = (int64_t *)(buf + sizeof(int64_t) + aid->di_offset);
	ptr[0] = value;
	ptr[1] = value;

	return 0;
}

int
dt_aggregate_go(dtrace_hdl_t *dtp)
{
	dt_aggregate_t	*agp = &dtp->dt_aggregate;
	dt_ahash_t	*agh = &agp->dtat_hash;
	int		aggsz, i;
	uint32_t	key = 0;

	/* If there are no aggregations there is nothing to do. */
	aggsz = dt_idhash_datasize(dtp->dt_aggs);
	if (aggsz <= 0)
		return 0;

	/*
	 * Allocate a buffer to hold the aggregation data for all possible
	 * CPUs, and initialize the per-CPU data pointers for CPUs that are
	 * currently enabled.
	 *
	 * The size of the aggregation data is to be increased by the size of
	 * the latch sequence number.  That yields the actual size of the data
	 * allocated per CPU.
	 */
	aggsz += sizeof(uint64_t);
	agp->dtat_buf = dt_zalloc(dtp, dtp->dt_conf.num_possible_cpus * aggsz);
	if (agp->dtat_buf == NULL)
		return dt_set_errno(dtp, EDT_NOMEM);

	agp->dtat_cpu_buf = dt_calloc(dtp, dtp->dt_conf.max_cpuid + 1,
				      sizeof(char *));
	if (agp->dtat_cpu_buf == NULL) {
		dt_free(dtp, agp->dtat_buf);
		return dt_set_errno(dtp, EDT_NOMEM);
	}

	for (i = 0; i < dtp->dt_conf.num_online_cpus; i++) {
		int	cpu = dtp->dt_conf.cpus[i].cpu_id;

		agp->dtat_cpu_buf[cpu] = agp->dtat_buf + cpu * aggsz;
	}

	/* Create the aggregation hash. */
	agh->dtah_size = DTRACE_AHASHSIZE;
	agh->dtah_hash = dt_zalloc(dtp,
				   agh->dtah_size * sizeof(dt_ahashent_t *));
	if (agh->dtah_hash == NULL) {
		dt_free(dtp, agp->dtat_cpu_buf);
		dt_free(dtp, agp->dtat_buf);
		return dt_set_errno(dtp, EDT_NOMEM);
	}

	/* Initialize the starting values for min() and max() aggregations.  */
	dt_idhash_iter(dtp->dt_aggs, (dt_idhash_f *) init_minmax,
		       agp->dtat_buf);
	if (*(int64_t *)agp->dtat_buf != 0) {
		*(int64_t *)agp->dtat_buf = 0;	/* clear the flag */

		for (i = 0; i < dtp->dt_conf.num_online_cpus; i++) {
			int	cpu = dtp->dt_conf.cpus[i].cpu_id;

			/* Data for CPU 0 was populated, so skip it. */
			if (cpu == 0)
				continue;

			memcpy(agp->dtat_cpu_buf[cpu], agp->dtat_buf, aggsz);
		}

		dt_bpf_map_update(dtp->dt_aggmap_fd, &key, agp->dtat_buf);
	}

	return 0;
}

static int
dt_aggwalk_rval(dtrace_hdl_t *dtp, dt_ahashent_t *h, int rval)
{
	dt_aggregate_t *agp = &dtp->dt_aggregate;
	dtrace_aggdata_t *data;
	dtrace_aggdesc_t *aggdesc;
	dtrace_recdesc_t *rec;
	int i;

	switch (rval) {
	case DTRACE_AGGWALK_NEXT:
		break;

	case DTRACE_AGGWALK_CLEAR: {
		uint32_t size, offs = 0;

		aggdesc = h->dtahe_data.dtada_desc;
		rec = &aggdesc->dtagd_recs[aggdesc->dtagd_nrecs - 1];
		size = rec->dtrd_size;
		data = &h->dtahe_data;

		if (rec->dtrd_action == DTRACEAGG_LQUANTIZE) {
			offs = sizeof(uint64_t);
			size -= sizeof(uint64_t);
		}

		memset(&data->dtada_data[rec->dtrd_offset] + offs, 0, size);

		if (data->dtada_percpu == NULL)
			break;

		for (i = 0; i <= dtp->dt_conf.max_cpuid; i++)
			memset(data->dtada_percpu[i] + offs, 0, size);
		break;
	}

	case DTRACE_AGGWALK_ERROR:
		/*
		 * We assume that errno is already set in this case.
		 */
		return dt_set_errno(dtp, errno);

	case DTRACE_AGGWALK_ABORT:
		return dt_set_errno(dtp, EDT_DIRABORT);

	case DTRACE_AGGWALK_DENORMALIZE:
		h->dtahe_data.dtada_normal = 1;
		return 0;

	case DTRACE_AGGWALK_NORMALIZE:
		if (h->dtahe_data.dtada_normal == 0) {
			h->dtahe_data.dtada_normal = 1;
			return dt_set_errno(dtp, EDT_BADRVAL);
		}

		return 0;

	case DTRACE_AGGWALK_REMOVE: {
		dtrace_aggdata_t *aggdata = &h->dtahe_data;
		int i, max_cpus = dtp->dt_conf.max_cpuid + 1;

		/*
		 * First, remove this hash entry from its hash chain.
		 */
		if (h->dtahe_prev != NULL) {
			h->dtahe_prev->dtahe_next = h->dtahe_next;
		} else {
			dt_ahash_t *hash = &agp->dtat_hash;
			size_t ndx = h->dtahe_hval % hash->dtah_size;

			assert(hash->dtah_hash[ndx] == h);
			hash->dtah_hash[ndx] = h->dtahe_next;
		}

		if (h->dtahe_next != NULL)
			h->dtahe_next->dtahe_prev = h->dtahe_prev;

		/*
		 * Now remove it from the list of all hash entries.
		 */
		if (h->dtahe_prevall != NULL) {
			h->dtahe_prevall->dtahe_nextall = h->dtahe_nextall;
		} else {
			dt_ahash_t *hash = &agp->dtat_hash;

			assert(hash->dtah_all == h);
			hash->dtah_all = h->dtahe_nextall;
		}

		if (h->dtahe_nextall != NULL)
			h->dtahe_nextall->dtahe_prevall = h->dtahe_prevall;

		/*
		 * We're unlinked.  We can safely destroy the data.
		 */
		if (aggdata->dtada_percpu != NULL) {
			for (i = 0; i < max_cpus; i++)
				dt_free(dtp, aggdata->dtada_percpu[i]);
			dt_free(dtp, aggdata->dtada_percpu);
		}

		dt_free(dtp, aggdata->dtada_data);
		dt_free(dtp, h);

		return 0;
	}

	default:
		return dt_set_errno(dtp, EDT_BADRVAL);
	}

	return 0;
}

void
dt_aggregate_qsort(dtrace_hdl_t *dtp, void *base, size_t nel, size_t width,
    int (*compar)(const void *, const void *))
{
	int rev = dt_revsort, key = dt_keysort, keypos = dt_keypos;
	dtrace_optval_t keyposopt = dtp->dt_options[DTRACEOPT_AGGSORTKEYPOS];

	dt_revsort = (dtp->dt_options[DTRACEOPT_AGGSORTREV] != DTRACEOPT_UNSET);
	dt_keysort = (dtp->dt_options[DTRACEOPT_AGGSORTKEY] != DTRACEOPT_UNSET);

	if (keyposopt != DTRACEOPT_UNSET && keyposopt <= INT_MAX) {
		dt_keypos = (int)keyposopt;
	} else {
		dt_keypos = 0;
	}

	if (compar == NULL) {
		if (!dt_keysort) {
			compar = dt_aggregate_varvalcmp;
		} else {
			compar = dt_aggregate_varkeycmp;
		}
	}

	qsort(base, nel, width, compar);

	dt_revsort = rev;
	dt_keysort = key;
	dt_keypos = keypos;
}

int
dtrace_aggregate_walk(dtrace_hdl_t *dtp, dtrace_aggregate_f *func, void *arg)
{
	dt_ahashent_t *h, *next;
	dt_ahash_t *hash = &dtp->dt_aggregate.dtat_hash;

	for (h = hash->dtah_all; h != NULL; h = next) {
		/*
		 * dt_aggwalk_rval() can potentially remove the current hash
		 * entry; we need to load the next hash entry before calling
		 * into it.
		 */
		next = h->dtahe_nextall;

		if (dt_aggwalk_rval(dtp, h, func(&h->dtahe_data, arg)) == -1)
			return -1;
	}

	return 0;
}

static int
dt_aggregate_walk_sorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
			 void *arg, int (*sfunc)(const void *, const void *))
{
	dt_aggregate_t *agp = &dtp->dt_aggregate;
	dt_ahashent_t *h, **sorted;
	dt_ahash_t *hash = &agp->dtat_hash;
	size_t i, nentries = 0;

	dtrace_aggregate_snap(dtp);

	/*
	 * Count how many aggregations have data in the buffers.  If there are
	 * aggregations but none have recorded data yet, there is nothing to do.
	 */
	for (h = hash->dtah_all; h != NULL; h = h->dtahe_nextall)
		nentries++;
	if (nentries == 0)
		return 0;

	sorted = dt_calloc(dtp, nentries, sizeof(dt_ahashent_t *));
	if (sorted == NULL)
		return -1;

	for (h = hash->dtah_all, i = 0; h != NULL; h = h->dtahe_nextall)
		sorted[i++] = h;

	pthread_mutex_lock(&dt_qsort_lock);

	if (sfunc == NULL) {
		dt_aggregate_qsort(dtp, sorted, nentries,
		    sizeof(dt_ahashent_t *), NULL);
	} else {
		/*
		 * If we've been explicitly passed a sorting function,
		 * we'll use that -- ignoring the values of the "aggsortrev",
		 * "aggsortkey" and "aggsortkeypos" options.
		 */
		qsort(sorted, nentries, sizeof(dt_ahashent_t *), sfunc);
	}

	pthread_mutex_unlock(&dt_qsort_lock);

	for (i = 0; i < nentries; i++) {
		h = sorted[i];

		if (dt_aggwalk_rval(dtp, h, func(&h->dtahe_data, arg)) == -1) {
			dt_free(dtp, sorted);
			return -1;
		}
	}

	dt_free(dtp, sorted);
	return 0;
}

int
dtrace_aggregate_walk_sorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
			     void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg, NULL);
}

int
dtrace_aggregate_walk_keysorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_varkeycmp);
}

int
dtrace_aggregate_walk_valsorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_varvalcmp);
}

int
dtrace_aggregate_walk_keyvarsorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				   void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_keyvarcmp);
}

int
dtrace_aggregate_walk_valvarsorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				   void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_valvarcmp);
}

int
dtrace_aggregate_walk_keyrevsorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				   void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_varkeyrevcmp);
}

int
dtrace_aggregate_walk_valrevsorted(dtrace_hdl_t *dtp, dtrace_aggregate_f *func,
				   void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_varvalrevcmp);
}

int
dtrace_aggregate_walk_keyvarrevsorted(dtrace_hdl_t *dtp,
				      dtrace_aggregate_f *func, void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_keyvarrevcmp);
}

int
dtrace_aggregate_walk_valvarrevsorted(dtrace_hdl_t *dtp,
				      dtrace_aggregate_f *func, void *arg)
{
	return dt_aggregate_walk_sorted(dtp, func, arg,
					dt_aggregate_valvarrevcmp);
}

int
dtrace_aggregate_walk_joined(dtrace_hdl_t *dtp, dtrace_aggid_t *aggvars,
			     int naggvars, dtrace_aggregate_walk_joined_f *func,
			     void *arg)
{
	dt_aggregate_t *agp = &dtp->dt_aggregate;
	dt_ahashent_t *h, **sorted = NULL, ***bundle, **nbundle;
	const dtrace_aggdata_t **data;
	dt_ahashent_t *zaggdata = NULL;
	dt_ahash_t *hash = &agp->dtat_hash;
	size_t nentries = 0, nbundles = 0, start, zsize = 0, bundlesize;
	dtrace_aggid_t max = 0, aggvar;
	int rval = -1, *map, *remap = NULL;
	int i, j;
	dtrace_optval_t sortpos = dtp->dt_options[DTRACEOPT_AGGSORTPOS];

	/*
	 * If the sorting position is greater than the number of aggregation
	 * variable IDs, we silently set it to 0.
	 */
	if (sortpos == DTRACEOPT_UNSET || sortpos >= naggvars)
		sortpos = 0;

	/*
	 * First we need to translate the specified aggregation variable IDs
	 * into a linear map that will allow us to translate an aggregation
	 * variable ID into its position in the specified aggvars.
	 */
	for (i = 0; i < naggvars; i++) {
		if (aggvars[i] == DTRACE_AGGVARIDNONE || aggvars[i] < 0)
			return dt_set_errno(dtp, EDT_BADAGGVAR);

		if (aggvars[i] > max)
			max = aggvars[i];
	}

	map = dt_calloc(dtp, max + 1, sizeof(int));
	if (map == NULL)
		return -1;

	zaggdata = dt_calloc(dtp, naggvars, sizeof(dt_ahashent_t));
	if (zaggdata == NULL)
		goto out;

	for (i = 0; i < naggvars; i++) {
		int ndx = i + sortpos;

		if (ndx >= naggvars)
			ndx -= naggvars;

		aggvar = aggvars[ndx];
		assert(aggvar <= max);

		if (map[aggvar]) {
			/*
			 * We have an aggregation variable that is present
			 * more than once in the array of aggregation
			 * variables.  While it's unclear why one might want
			 * to do this, it's legal.  To support this construct,
			 * we will allocate a remap that will indicate the
			 * position from which this aggregation variable
			 * should be pulled.  (That is, where the remap will
			 * map from one position to another.)
			 */
			if (remap == NULL) {
				remap = dt_calloc(dtp, naggvars, sizeof(int));
				if (remap == NULL)
					goto out;
			}

			/*
			 * Given that the variable is already present, assert
			 * that following through the mapping and adjusting
			 * for the sort position yields the same aggregation
			 * variable ID.
			 */
			assert(aggvars[(map[aggvar] - 1 + sortpos) %
			    naggvars] == aggvars[ndx]);

			remap[i] = map[aggvar];
			continue;
		}

		map[aggvar] = i + 1;
	}

	/*
	 * Retrieve the aggregation data.
	 */
	dtrace_aggregate_snap(dtp);

	/*
	 * We need to take two passes over the data to size our allocation, so
	 * we'll use the first pass to also fill in the zero-filled data to be
	 * used to properly format a zero-valued aggregation.
	 */
	for (h = hash->dtah_all; h != NULL; h = h->dtahe_nextall) {
		dtrace_aggid_t	id;
		int		ndx;

		if ((id = dt_aggregate_aggid(h)) > max || !(ndx = map[id]))
			continue;

		if (zaggdata[ndx - 1].dtahe_size == 0) {
			zaggdata[ndx - 1].dtahe_size = h->dtahe_size;
			zaggdata[ndx - 1].dtahe_data = h->dtahe_data;
		}

		nentries++;
	}

	if (nentries == 0) {
		/*
		 * We couldn't find any entries; there is nothing else to do.
		 */
		rval = 0;
		goto out;
	}

	/*
	 * Before we sort the data, we're going to look for any holes in our
	 * zero-filled data.  This will occur if an aggregation variable that
	 * we are being asked to print has not yet been assigned the result of
	 * any aggregating action for _any_ tuple.  The issue becomes that we
	 * would like a zero value to be printed for all columns for this
	 * aggregation, but without any record description, we don't know the
	 * aggregating action that corresponds to the aggregation variable.  To
	 * try to find a match, we're simply going to lookup aggregation IDs
	 * (which are guaranteed to be contiguous and to start from 1), looking
	 * for the specified aggregation variable ID.  If we find a match,
	 * we'll use that.  If we iterate over all aggregation IDs and don't
	 * find a match, then we must be an anonymous enabling.  (Anonymous
	 * enablings can't currently derive either aggregation variable IDs or
	 * aggregation variable names given only an aggregation ID.)  In this
	 * obscure case (anonymous enabling, multiple aggregation printa() with
	 * some aggregations not represented for any tuple), our defined
	 * behavior is that the zero will be printed in the format of the first
	 * aggregation variable that contains any non-zero value.
	 */
	for (i = 0; i < naggvars; i++) {
		if (zaggdata[i].dtahe_size == 0) {
			dtrace_aggid_t	aggvar;

			aggvar = aggvars[(i - sortpos + naggvars) % naggvars];
			assert(zaggdata[i].dtahe_data.dtada_data == NULL);

			for (j = DTRACE_AGGIDNONE + 1; ; j++) {
				dtrace_aggdesc_t *agg;
				dtrace_aggdata_t *aggdata;

				if (dt_aggid_lookup(dtp, j, &agg) != 0)
					break;

				if (agg->dtagd_varid != aggvar)
					continue;

				/*
				 * We have our description -- now we need to
				 * cons up the zaggdata entry for it.
				 */
				aggdata = &zaggdata[i].dtahe_data;
				aggdata->dtada_size = agg->dtagd_size;
				aggdata->dtada_desc = agg;
				aggdata->dtada_hdl = dtp;
				aggdata->dtada_normal = 1;
				zaggdata[i].dtahe_hval = 0;
				zaggdata[i].dtahe_size = agg->dtagd_size;
				break;
			}

			if (zaggdata[i].dtahe_size == 0) {
				caddr_t data;

				/*
				 * We couldn't find this aggregation, meaning
				 * that we have never seen it before for any
				 * tuple _and_ this is an anonymous enabling.
				 * That is, we're in the obscure case outlined
				 * above.  In this case, our defined behavior
				 * is to format the data in the format of the
				 * first non-zero aggregation -- of which, of
				 * course, we know there to be at least one
				 * (or nentries would have been zero).
				 */
				for (j = 0; j < naggvars; j++) {
					if (zaggdata[j].dtahe_size != 0)
						break;
				}

				assert(j < naggvars);
				zaggdata[i] = zaggdata[j];

				data = zaggdata[i].dtahe_data.dtada_data;
				assert(data != NULL);
			}
		}
	}

	/*
	 * Now we need to allocate our zero-filled data for use for
	 * aggregations that don't have a value corresponding to a given key.
	 */
	for (i = 0; i < naggvars; i++) {
		dtrace_aggdata_t *aggdata = &zaggdata[i].dtahe_data;
		caddr_t zdata;

		zsize = zaggdata[i].dtahe_size;
		assert(zsize != 0);

		if ((zdata = dt_zalloc(dtp, zsize)) == NULL) {
			/*
			 * If we failed to allocated some zero-filled data, we
			 * need to zero out the remaining dtada_data pointers
			 * to prevent the wrong data from being freed below.
			 */
			for (j = i; j < naggvars; j++)
				zaggdata[j].dtahe_data.dtada_data = NULL;
			goto out;
		}

		aggvar = aggvars[(i - sortpos + naggvars) % naggvars];
		aggdata->dtada_data = zdata;
	}

	/*
	 * Now that we've dealt with setting up our zero-filled data, we can
	 * allocate our sorted array, and take another pass over the data to
	 * fill it.
	 */
	sorted = dt_calloc(dtp, nentries, sizeof(dt_ahashent_t *));
	if (sorted == NULL)
		goto out;

	for (h = hash->dtah_all, i = 0; h != NULL; h = h->dtahe_nextall) {
		dtrace_aggid_t	id;

		if ((id = dt_aggregate_aggid(h)) > max || !map[id])
			continue;

		sorted[i++] = h;
	}

	assert(i == nentries);

	/*
	 * We've loaded our array; now we need to sort by value to allow us
	 * to create bundles of like value.  We're going to acquire the
	 * dt_qsort_lock here, and hold it across all of our subsequent
	 * comparison and sorting.
	 */
	pthread_mutex_lock(&dt_qsort_lock);

	qsort(sorted, nentries, sizeof(dt_ahashent_t *),
	    dt_aggregate_keyvarcmp);

	/*
	 * Now we need to go through and create bundles.  Because the number
	 * of bundles is bounded by the size of the sorted array, we're going
	 * to reuse the underlying storage.  And note that "bundle" is an
	 * array of pointers to arrays of pointers to dt_ahashent_t -- making
	 * its type (regrettably) "dt_ahashent_t ***".  (Regrettable because
	 * '*' -- like '_' and 'X' -- should never appear in triplicate in
	 * an ideal world.)
	 */
	bundle = (dt_ahashent_t ***)sorted;

	for (i = 1, start = 0; i <= nentries; i++) {
		if (i < nentries &&
		    dt_aggregate_keycmp(&sorted[i], &sorted[i - 1]) == 0)
			continue;

		/*
		 * We have a bundle boundary.  Everything from start to
		 * (i - 1) belongs in one bundle.
		 */
		assert(i - start <= naggvars);
		bundlesize = (naggvars + 2) * sizeof(dt_ahashent_t *);

		if ((nbundle = dt_zalloc(dtp, bundlesize)) == NULL) {
			pthread_mutex_unlock(&dt_qsort_lock);
			goto out;
		}

		for (j = start; j < i; j++) {
			dtrace_aggid_t	id = dt_aggregate_aggid(sorted[j]);

			assert(id <= max);
			assert(map[id] != 0);
			assert(map[id] - 1 < naggvars);
			assert(nbundle[map[id] - 1] == NULL);
			nbundle[map[id] - 1] = sorted[j];

			if (nbundle[naggvars] == NULL)
				nbundle[naggvars] = sorted[j];
		}

		for (j = 0; j < naggvars; j++) {
			if (nbundle[j] != NULL)
				continue;

			/*
			 * Before we assume that this aggregation variable
			 * isn't present (and fall back to using the
			 * zero-filled data allocated earlier), check the
			 * remap.  If we have a remapping, we'll drop it in
			 * here.  Note that we might be remapping an
			 * aggregation variable that isn't present for this
			 * key; in this case, the aggregation data that we
			 * copy will point to the zeroed data.
			 */
			if (remap != NULL && remap[j]) {
				assert(remap[j] - 1 < j);
				assert(nbundle[remap[j] - 1] != NULL);
				nbundle[j] = nbundle[remap[j] - 1];
			} else
				nbundle[j] = &zaggdata[j];
		}

		bundle[nbundles++] = nbundle;
		start = i;
	}

	/*
	 * Now we need to re-sort based on the first value.
	 */
	dt_aggregate_qsort(dtp, bundle, nbundles, sizeof(dt_ahashent_t **),
	    dt_aggregate_bundlecmp);

	pthread_mutex_unlock(&dt_qsort_lock);

	/*
	 * We're done!  Now we just need to go back over the sorted bundles,
	 * calling the function.
	 */
	data = alloca((naggvars + 1) * sizeof(dtrace_aggdata_t *));

	for (i = 0; i < nbundles; i++) {
		for (j = 0; j < naggvars; j++)
			data[j + 1] = NULL;

		for (j = 0; j < naggvars; j++) {
			int ndx = j - sortpos;

			if (ndx < 0)
				ndx += naggvars;

			assert(bundle[i][ndx] != NULL);
			data[j + 1] = &bundle[i][ndx]->dtahe_data;
		}

		for (j = 0; j < naggvars; j++)
			assert(data[j + 1] != NULL);

		/*
		 * The representative key is the last element in the bundle.
		 * Assert that we have one, and then set it to be the first
		 * element of data.
		 */
		assert(bundle[i][j] != NULL);
		data[0] = &bundle[i][j]->dtahe_data;

		if ((rval = func(data, naggvars + 1, arg)) == -1)
			goto out;
	}

	rval = 0;
out:
	for (i = 0; i < nbundles; i++)
		dt_free(dtp, bundle[i]);

	if (zaggdata != NULL) {
		for (i = 0; i < naggvars; i++)
			dt_free(dtp, zaggdata[i].dtahe_data.dtada_data);
	}

	dt_free(dtp, zaggdata);
	dt_free(dtp, sorted);
	dt_free(dtp, remap);
	dt_free(dtp, map);

	return rval;
}

int
dtrace_aggregate_print(dtrace_hdl_t *dtp, FILE *fp,
    dtrace_aggregate_walk_f *func)
{
	dtrace_print_aggdata_t pd;

	if (dt_idhash_datasize(dtp->dt_aggs) == 0)
		return 0;

	pd.dtpa_dtp = dtp;
	pd.dtpa_fp = fp;
	pd.dtpa_allunprint = 1;

	if (func == NULL)
		func = dtrace_aggregate_walk_sorted;

	if ((*func)(dtp, dt_print_agg, &pd) == -1)
		return dt_set_errno(dtp, dtp->dt_errno);

	return 0;
}

void
dtrace_aggregate_clear(dtrace_hdl_t *dtp)
{
	dt_aggregate_t *agp = &dtp->dt_aggregate;
	dt_ahash_t *hash = &agp->dtat_hash;
	dt_ahashent_t *h;
	dtrace_aggdata_t *data;
	dtrace_aggdesc_t *aggdesc;
	dtrace_recdesc_t *rec;
	int i, max_cpus = dtp->dt_conf.max_cpuid + 1;

	for (h = hash->dtah_all; h != NULL; h = h->dtahe_nextall) {
		aggdesc = h->dtahe_data.dtada_desc;
		rec = &aggdesc->dtagd_recs[aggdesc->dtagd_nrecs - 1];
		data = &h->dtahe_data;

		memset(&data->dtada_data[rec->dtrd_offset], 0, rec->dtrd_size);

		if (data->dtada_percpu == NULL)
			continue;

		for (i = 0; i < max_cpus; i++)
			memset(data->dtada_percpu[i], 0, rec->dtrd_size);
	}
}

void
dt_aggregate_destroy(dtrace_hdl_t *dtp)
{
	dt_aggregate_t *agp = &dtp->dt_aggregate;
	dt_ahash_t *hash = &agp->dtat_hash;
	dt_ahashent_t *h, *next;
	dtrace_aggdata_t *aggdata;
	int i, max_cpus = dtp->dt_conf.max_cpuid + 1;

	if (hash->dtah_hash == NULL) {
		assert(hash->dtah_all == NULL);
	} else {
		dt_free(dtp, hash->dtah_hash);

		for (h = hash->dtah_all; h != NULL; h = next) {
			next = h->dtahe_nextall;

			aggdata = &h->dtahe_data;

			if (aggdata->dtada_percpu != NULL) {
				for (i = 0; i < max_cpus; i++)
					dt_free(dtp, aggdata->dtada_percpu[i]);
				dt_free(dtp, aggdata->dtada_percpu);
			}

			dt_free(dtp, aggdata->dtada_data);
			dt_free(dtp, h);
		}

		hash->dtah_hash = NULL;
		hash->dtah_all = NULL;
		hash->dtah_size = 0;
	}

	dt_free(dtp, agp->dtat_cpu_buf);
	dt_free(dtp, agp->dtat_buf);
}
