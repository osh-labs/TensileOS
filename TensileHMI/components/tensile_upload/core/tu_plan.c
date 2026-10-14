/* tensile_upload core: upload request planner. Pure C11.
 *
 * Request sequence for "Tests/YYYY-MM-DD/name.csv":
 *   MKCOL Tests/YYYY-MM-DD/      201 or 405 -> PUT
 *                                409 (parent missing) -> MKCOL Tests/ -> MKCOL Tests/YYYY-MM-DD/ -> PUT
 *   PUT .../name.csv  If-None-Match: *
 *                                201/204 -> DONE
 *                                412 -> PUT .../name_TAB5.csv -> 412 -> PUT .../name_TAB5_2.csv ...
 *                                404/409 (dirs_known was wrong) -> MKCOL phase once, then PUT again
 *   With If-Match: <etag>, a 412 switches to create-only "<name>_hmi-edit.csv"
 *   (build plan 6.2 step 3).
 *   AUTH_FAILED / RETRY_LATER / PERMANENT_ERROR end the plan in FAILED; the caller
 *   (queue) decides whether to retry the whole plan later.
 */
#include "tu_core.h"

#include <string.h>

static void set_dir(tu_plan_t *p, int idx)
{
    p->dir_idx = idx;
    size_t n = p->dir_end[idx];
    memcpy(p->dir_buf, p->orig_path, n);
    p->dir_buf[n] = '\0';
}

static void fail(tu_plan_t *p, const tu_class_t *c)
{
    p->fail = *c;
    p->state = TU_PS_FAILED;
}

static int rename_for_collision(tu_plan_t *p)
{
    int rc = tu_collision_name(p->orig_path, p->suffix, p->collision, p->path, sizeof p->path);
    if (rc != TU_OK) {
        tu_class_t c = {TU_RES_PERMANENT_ERROR, p->last.http_status, -1};
        fail(p, &c);
    }
    return rc;
}

int tu_plan_init(tu_plan_t *p, const char *remote_path, const tu_plan_opts_t *opts)
{
    if (!p || !remote_path) return TU_ERR_ARG;
    memset(p, 0, sizeof *p);
    p->state = TU_PS_FAILED;
    p->fail.result = TU_RES_PERMANENT_ERROR;
    p->fail.retry_after_s = -1;
    p->last.retry_after_s = -1;

    while (*remote_path == '/') remote_path++;
    size_t len = strlen(remote_path);
    if (len == 0 || len >= TU_PATH_MAX) return TU_ERR_ARG;
    if (remote_path[len - 1] == '/') return TU_ERR_PATH;

    /* validate segments, record ancestor prefixes */
    const char *seg = remote_path;
    for (size_t i = 0; i <= len; i++) {
        char ch = remote_path[i];
        if (ch == '/' || ch == '\0') {
            size_t n = (size_t)(remote_path + i - seg);
            if (n == 0) return TU_ERR_PATH;
            if ((n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) return TU_ERR_PATH;
            if (ch == '/') {
                if (p->n_dirs >= TU_PLAN_MAX_DEPTH) return TU_ERR_PATH;
                p->dir_end[p->n_dirs++] = (uint16_t)(i + 1); /* include trailing '/' */
            }
            seg = remote_path + i + 1;
        } else if ((unsigned char)ch < 0x20 || ch == 0x7F || ch == '\\') {
            return TU_ERR_PATH;
        }
    }
    memcpy(p->orig_path, remote_path, len + 1);
    memcpy(p->path, remote_path, len + 1);

    p->precond = opts ? opts->precond : TU_PRECOND_IF_NONE_MATCH_STAR;
    if (p->precond == TU_PRECOND_IF_MATCH) {
        if (!opts->etag || !*opts->etag || strlen(opts->etag) >= TU_ETAG_MAX) return TU_ERR_ARG;
        strcpy(p->etag, opts->etag);
    }
    p->max_collisions = (opts && opts->max_collisions) ? opts->max_collisions : TU_PLAN_MAX_COLLISIONS;
    p->suffix = TU_COLLISION_SUFFIX;
    p->dir_ok = -1;

    if (p->n_dirs == 0 || (opts && opts->dirs_known)) {
        p->mkcol_done = (p->n_dirs == 0);
        p->state = TU_PS_PUT;
    } else {
        p->state = TU_PS_MKCOL;
        set_dir(p, p->n_dirs - 1);
    }
    return TU_OK;
}

tu_step_t tu_plan_step(const tu_plan_t *p)
{
    tu_step_t s = {TU_STEP_FAILED, NULL, TU_PRECOND_NONE, NULL};
    if (!p) return s;
    switch (p->state) {
    case TU_PS_MKCOL:
        s.kind = TU_STEP_MKCOL;
        s.path = p->dir_buf;
        break;
    case TU_PS_PUT:
        s.kind = TU_STEP_PUT;
        s.path = p->path;
        s.precond = p->precond;
        s.etag = (p->precond == TU_PRECOND_IF_MATCH) ? p->etag : NULL;
        break;
    case TU_PS_DONE:
        s.kind = TU_STEP_DONE;
        s.path = p->path;
        break;
    default: break;
    }
    return s;
}

int tu_plan_feed(tu_plan_t *p, const tu_class_t *c)
{
    if (!p || !c) return TU_ERR_ARG;
    if (p->state == TU_PS_DONE || p->state == TU_PS_FAILED) return TU_ERR_STATE;
    p->last = *c;
    if (++p->steps > TU_PLAN_MAX_STEPS) {
        tu_class_t e = {TU_RES_PERMANENT_ERROR, c->http_status, -1};
        fail(p, &e);
        return TU_OK;
    }

    if (p->state == TU_PS_MKCOL) {
        switch (c->result) {
        case TU_RES_CREATED:
        case TU_RES_ALREADY_EXISTS:
        case TU_RES_OK:
            if (p->dir_idx > p->dir_ok) p->dir_ok = p->dir_idx;
            if (p->dir_idx >= p->n_dirs - 1) {
                p->mkcol_done = true;
                p->state = TU_PS_PUT;
            } else {
                set_dir(p, p->dir_idx + 1);
            }
            break;
        case TU_RES_NOT_FOUND:
            /* parent missing; if the parent was just confirmed, something odd
             * (e.g. a file with the folder's name) blocks us: give up. */
            if (p->dir_idx == 0 || p->dir_ok >= p->dir_idx - 1) fail(p, c);
            else set_dir(p, p->dir_idx - 1);
            break;
        default: fail(p, c); break;
        }
        return TU_OK;
    }

    /* PUT */
    switch (c->result) {
    case TU_RES_CREATED:
        p->created = true;
        p->state = TU_PS_DONE;
        break;
    case TU_RES_OK:
        p->state = TU_PS_DONE;
        break;
    case TU_RES_PRECONDITION_FAILED:
        if (p->precond == TU_PRECOND_IF_MATCH) {
            /* edited in Nextcloud meanwhile: keep both, upload ours as <name>_hmi-edit */
            p->precond = TU_PRECOND_IF_NONE_MATCH_STAR;
            p->etag[0] = '\0';
            p->suffix = TU_EDIT_SUFFIX;
            p->collision = 1;
            rename_for_collision(p);
        } else if (p->precond == TU_PRECOND_IF_NONE_MATCH_STAR) {
            if (++p->collision > p->max_collisions) fail(p, c);
            else rename_for_collision(p);
        } else {
            fail(p, c);
        }
        break;
    case TU_RES_NOT_FOUND:
        if (!p->mkcol_done && p->n_dirs > 0) {
            p->state = TU_PS_MKCOL;
            set_dir(p, p->n_dirs - 1);
        } else {
            fail(p, c);
        }
        break;
    default: fail(p, c); break;
    }
    return TU_OK;
}
