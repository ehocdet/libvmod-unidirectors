/*-
 * Copyright (c) 2013-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@FreeBSD.org>
 *
 * Copyright (c) 2016-2018 GANDI SAS
 * All rights reserved.
 *
 * Author: Emmanuel Hocdet <manu@gandi.net>
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
 */

#include "config.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "cache/cache.h"

#include "udir.h"
#include "dynamic.h"

struct vmod_director_round_robin {
	unsigned				magic;
#define VMOD_DIRECTOR_ROUND_ROBIN_MAGIC         0xe9537153
	pthread_mutex_t			        mtx;
	double				        w;
};

static void v_matchproto_(vdi_destroy_f)
rr_vdi_destroy(VCL_BACKEND dir)
{
	struct vmod_unidirectors_director *vd;
        struct vmod_director_round_robin *rr;
	CHECK_OBJ_NOTNULL(dir, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(vd, dir->priv, VMOD_UNIDIRECTORS_DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(rr, vd->priv, VMOD_DIRECTOR_ROUND_ROBIN_MAGIC);
	AZ(pthread_mutex_destroy(&rr->mtx));
	FREE_OBJ(rr);
}

static VCL_BACKEND v_matchproto_(vdi_resolve_f)
rr_vdi_resolve(VRT_CTX, VCL_BACKEND dir)
{
	struct vmod_unidirectors_director *vd;
        struct vmod_director_round_robin *rr;
	unsigned u, h, n_backend = 0;
	double w, tw = 0.0;
	be_idx_t *be_idx = NULL;
	VCL_BACKEND be, rbe = NULL;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(dir, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(vd, dir->priv, VMOD_UNIDIRECTORS_DIRECTOR_MAGIC);

	udir_rdlock(vd);
	CAST_OBJ_NOTNULL(rr, vd->priv, VMOD_DIRECTOR_ROUND_ROBIN_MAGIC);

	if (WS_ReserveAll(ctx->ws) >= vd->n_backend * sizeof(*be_idx)) {
		be_idx = (void*)ctx->ws->f;
		for (u = 0; u < vd->n_backend; u++) {
			be = vd->backend[u];
			CHECK_OBJ_NOTNULL(be, DIRECTOR_MAGIC);
			if (VRT_Healthy(ctx, be, NULL)) {
				be_idx[n_backend++] = u;
				tw += vd->weight[u];
			}
		}
	} else
		VRT_fail(ctx, "%s: Workspace overflow on vdi_resolve", vd->vcl_name);
	if (tw > 0.0) {
		double i;
		AN(be_idx);
		AZ(pthread_mutex_lock(&rr->mtx));
		w = modf(rr->w, &i);
		h = w * n_backend;
		u = be_idx[h];
		assert(u < vd->n_backend);
		rr->w = w + (1.0 - vd->weight[u] / tw);
		AZ(pthread_mutex_unlock(&rr->mtx));
		rbe = vd->backend[u];
		CHECK_OBJ_NOTNULL(rbe, DIRECTOR_MAGIC);
	}
	WS_Release(ctx->ws, 0);
	udir_unlock(vd);
	return (rbe);
}

static const struct vdi_methods rr_methods[1] = {{
	.magic =		VDI_METHODS_MAGIC,
	.type =			"round-robin",
	.healthy =		udir_vdi_healthy,
	.resolve =		rr_vdi_resolve,
	.find =			udir_vdi_find,
	.uptime =		udir_vdi_uptime,
	.destroy =		rr_vdi_destroy,
	.list =                 udir_vdi_list,
}};

VCL_VOID v_matchproto_()
vmod_director_round_robin(VRT_CTX, struct vmod_unidirectors_director *vd)
{
        struct vmod_director_round_robin *rr;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vd, VMOD_UNIDIRECTORS_DIRECTOR_MAGIC);

	if (vd->dir) {
		VRT_fail(ctx, "%s: LB method is already set", vd->vcl_name);
		return;
	}
	udir_wrlock(vd);
	ALLOC_OBJ(rr, VMOD_DIRECTOR_ROUND_ROBIN_MAGIC);
	vd->priv = rr;
	AN(vd->priv);
	AZ(pthread_mutex_init(&rr->mtx, NULL));

	vd->dir = VRT_AddDirector(ctx, rr_methods, vd, "%s", vd->vcl_name);

	udir_unlock(vd);
}

VCL_VOID v_matchproto_()
vmod_dyndirector_round_robin(VRT_CTX, struct vmod_unidirectors_dyndirector *dyn)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(dyn, VMOD_UNIDIRECTORS_DYNDIRECTOR_MAGIC);
	vmod_director_round_robin(ctx, dyn->vd);
}
