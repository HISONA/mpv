/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/types.h>

#include <libavutil/avstring.h>
#include <libavutil/common.h>

#include "config.h"
#include "mpa_talloc.h"
#include "client.h"
#include "osd.h"

#include "common/av_common.h"
#include "common/codecs.h"
#include "common/msg.h"
#include "common/msg_control.h"
#include "filters/f_decoder_wrapper.h"
#include "command.h"
#include "osdep/timer.h"
#include "common/common.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "stream/stream.h"
#include "demux/demux.h"
#include "demux/stheader.h"
#include "common/playlist.h"
#include "options/m_option.h"
#include "options/m_property.h"
#include "options/m_config.h"

#include "audio/aframe.h"
#include "audio/format.h"
#include "audio/out/ao.h"
#include "options/path.h"
#include "misc/dispatch.h"
#include "misc/node.h"
#include "misc/thread_pool.h"

#include "osdep/io.h"
#include "osdep/subprocess.h"

#include "core.h"

#ifdef _WIN32
#include <windows.h>
#endif

struct command_ctx {
    // All properties, terminated with a {0} item.
    struct m_property *properties;

    bool is_idle;

    double last_seek_time;
    double last_seek_pts;
    double marked_pts;

    char **warned_deprecated;
    int num_warned_deprecated;

    struct hook_handler **hooks;
    int num_hooks;
    int64_t hook_seq; // for hook_handler.seq

    struct ao_hotplug *hotplug;

    char *cur_ipc;
    char *cur_ipc_input;

    int silence_option_deprecations;
};


struct hook_handler {
    char *client;   // client API user name
    char *type;     // kind of hook, e.g. "on_load"
    uint64_t user_id; // user-chosen ID
    int priority;   // priority for global hook order
    int64_t seq;    // unique ID, != 0, also for fixed order on equal priorities
    bool legacy;    // old cmd based hook API
    bool active;    // hook is currently in progress (only 1 at a time for now)
};

// U+279C HEAVY ROUND-TIPPED RIGHTWARDS ARROW
// U+00A0 NO-BREAK SPACE
#define ARROW_SP "\342\236\234\302\240"

const char list_current[] = OSD_ASS_0 ARROW_SP OSD_ASS_1;
const char list_normal[] = OSD_ASS_0 "{\\alpha&HFF}" ARROW_SP "{\\r}" OSD_ASS_1;

static int set_filters(struct MPContext *mpctx, enum stream_type mediatype,
                       struct m_obj_settings *new_chain);

static int mp_property_do_silent(const char *name, int action, void *val,
                                 struct MPContext *ctx);

static void hook_remove(struct MPContext *mpctx, struct hook_handler *h)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    for (int n = 0; n < cmd->num_hooks; n++) {
        if (cmd->hooks[n] == h) {
            talloc_free(cmd->hooks[n]);
            MP_TARRAY_REMOVE_AT(cmd->hooks, cmd->num_hooks, n);
            return;
        }
    }
    assert(0);
}

bool mp_hook_test_completion(struct MPContext *mpctx, char *type)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    for (int n = 0; n < cmd->num_hooks; n++) {
        struct hook_handler *h = cmd->hooks[n];
        if (h->active && strcmp(h->type, type) == 0) {
            if (!mp_client_exists(mpctx, h->client)) {
                MP_WARN(mpctx, "client removed during hook handling\n");
                hook_remove(mpctx, h);
                break;
            }
            return false;
        }
    }
    return true;
}

static int invoke_hook_handler(struct MPContext *mpctx, struct hook_handler *h)
{
    MP_VERBOSE(mpctx, "Running hook: %s/%s\n", h->client, h->type);
    h->active = true;

    uint64_t reply_id = 0;
    void *data;
    int msg;
    if (h->legacy) {
        mpv_event_client_message *m = talloc_ptrtype(NULL, m);
        *m = (mpv_event_client_message){0};
        MP_TARRAY_APPEND(m, m->args, m->num_args, "hook_run");
        MP_TARRAY_APPEND(m, m->args, m->num_args,
                         talloc_asprintf(m, "%llu", (long long)h->user_id));
        MP_TARRAY_APPEND(m, m->args, m->num_args,
                         talloc_asprintf(m, "%llu", (long long)h->seq));
        data = m;
        msg = MPV_EVENT_CLIENT_MESSAGE;
    } else {
        mpv_event_hook *m = talloc_ptrtype(NULL, m);
        *m = (mpv_event_hook){
            .name = talloc_strdup(m, h->type),
            .id = h->seq,
        },
        reply_id = h->user_id;
        data = m;
        msg = MPV_EVENT_HOOK;
    }
    int r = mp_client_send_event(mpctx, h->client, reply_id, msg, data);
    if (r < 0) {
        MP_WARN(mpctx, "Sending hook command failed. Removing hook.\n");
        hook_remove(mpctx, h);
        mp_wakeup_core(mpctx); // repeat next iteration to finish
    }
    return r;
}

static int run_next_hook_handler(struct MPContext *mpctx, char *type, int index)
{
    struct command_ctx *cmd = mpctx->command_ctx;

    for (int n = index; n < cmd->num_hooks; n++) {
        struct hook_handler *h = cmd->hooks[n];
        if (strcmp(h->type, type) == 0)
            return invoke_hook_handler(mpctx, h);
    }

    mp_wakeup_core(mpctx); // finished hook
    return 0;
}

// Start processing script/client API hooks. This is asynchronous, and the
// caller needs to use mp_hook_test_completion() to check whether they're done.
void mp_hook_start(struct MPContext *mpctx, char *type)
{
    while (run_next_hook_handler(mpctx, type, 0) < 0) {
        // We can repeat this until all broken clients have been removed, and
        // hook processing is successfully started.
    }
}

int mp_hook_continue(struct MPContext *mpctx, char *client, uint64_t id)
{
    struct command_ctx *cmd = mpctx->command_ctx;

    for (int n = 0; n < cmd->num_hooks; n++) {
        struct hook_handler *h = cmd->hooks[n];
        if (strcmp(h->client, client) == 0 && h->seq == id) {
            if (!h->active)
                break;
            h->active = false;
            return run_next_hook_handler(mpctx, h->type, n + 1);
        }
    }

    MP_ERR(mpctx, "invalid hook API usage\n");
    return MPV_ERROR_INVALID_PARAMETER;
}

static int compare_hook(const void *pa, const void *pb)
{
    struct hook_handler **h1 = (void *)pa;
    struct hook_handler **h2 = (void *)pb;
    if ((*h1)->priority != (*h2)->priority)
        return (*h1)->priority - (*h2)->priority;
    return (*h1)->seq - (*h2)->seq;
}

void mp_hook_add(struct MPContext *mpctx, const char *client, const char *name,
                 uint64_t user_id, int pri, bool legacy)
{
    if (legacy)
        MP_WARN(mpctx, "The old hook API is deprecated! Use the libmpv API.\n");

    struct command_ctx *cmd = mpctx->command_ctx;
    struct hook_handler *h = talloc_ptrtype(cmd, h);
    int64_t seq = ++cmd->hook_seq;
    *h = (struct hook_handler){
        .client = talloc_strdup(h, client),
        .type = talloc_strdup(h, name),
        .user_id = user_id,
        .priority = pri,
        .seq = seq,
        .legacy = legacy,
    };
    MP_TARRAY_APPEND(cmd, cmd->hooks, cmd->num_hooks, h);
    qsort(cmd->hooks, cmd->num_hooks, sizeof(cmd->hooks[0]), compare_hook);
}

// Call before a seek, in order to allow revert-seek to undo the seek.
void mark_seek(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;
    double now = mp_time_sec();
    if (now > cmd->last_seek_time + 2.0 || cmd->last_seek_pts == MP_NOPTS_VALUE)
        cmd->last_seek_pts = get_current_time(mpctx);
    cmd->last_seek_time = now;
}

static char *format_delay(double time)
{
    return talloc_asprintf(NULL, "%d ms", (int)lrint(time * 1000));
}

// Option-property bridge. This is used so that setting options via various
// mechanisms (including command line parsing, config files, per-file options)
// updates state associated with them. For that, they have to go through the
// property layer. (Ideally, this would be the other way around, and there
// would be per-option change handlers instead.)
// Note that the property-option bridge sidesteps this, as we'd get infinite
// recursion.
int mp_on_set_option(void *ctx, struct m_config_option *co, void *data, int flags)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    const char *name = co->name;

    // Skip going through mp_property_generic_option (typically), because the
    // property implementation is trivial, and can break some obscure features
    // like --profile and --include if non-trivial flags are involved (which
    // the bridge would drop).
    struct m_property *prop = m_property_list_find(cmd->properties, name);
    if (prop && prop->is_option)
        goto direct_option;

    struct m_option type = {0};

    int r = mp_property_do_silent(name, M_PROPERTY_GET_TYPE, &type, mpctx);
    if (r == M_PROPERTY_UNKNOWN)
        goto direct_option; // not mapped as property
    if (r != M_PROPERTY_OK)
        return M_OPT_INVALID; // shouldn't happen

    assert(type.type == co->opt->type);
    assert(type.max == co->opt->max);
    assert(type.min == co->opt->min);

    r = mp_property_do_silent(name, M_PROPERTY_SET, data, mpctx);
    if (r != M_PROPERTY_OK)
        return M_OPT_INVALID;

    // The flags can't be passed through the property layer correctly.
    m_config_mark_co_flags(co, flags);

    return 0;

direct_option:
    mp_notify_property(mpctx, name);
    return m_config_set_option_raw_direct(mpctx->mconfig, co, data, flags);
}

// Property-option bridge. (Maps the property to the option with the same name.)
static int mp_property_generic_option(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    const char *optname = prop->name;
    int flags = M_SETOPT_RUNTIME;
    struct m_config_option *opt;
    if (mpctx->command_ctx->silence_option_deprecations) {
        // This case is specifically for making --reset-on-next-file=all silent.
        opt = m_config_get_co_raw(mpctx->mconfig, bstr0(optname));
    } else {
        opt = m_config_get_co(mpctx->mconfig, bstr0(optname));
    }

    if (!opt)
        return M_PROPERTY_UNKNOWN;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = *(opt->opt);
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
        if (!opt->data)
            return M_PROPERTY_NOT_IMPLEMENTED;
        m_option_copy(opt->opt, arg, opt->data);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET:
        if (m_config_set_option_raw_direct(mpctx->mconfig, opt, arg, flags) < 0)
            return M_PROPERTY_ERROR;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// Playback speed (RW)
static int mp_property_playback_speed(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    double speed = mpctx->opts->playback_speed;
    switch (action) {
    case M_PROPERTY_SET: {
        int r = mp_property_generic_option(mpctx, prop, action, arg);
        update_playback_speed(mpctx);
        mp_wakeup_core(mpctx);
        return r;
    }
    case M_PROPERTY_PRINT:
        *(char **)arg = talloc_asprintf(NULL, "%.2f", speed);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_av_speed_correction(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    char *type = prop->priv;
    double val = 0;
    switch (type[0]) {
    case 'a': val = mpctx->speed_factor_a; break;
    case 'v': val = mpctx->speed_factor_v; break;
    default: abort();
    }

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%+.05f%%", (val - 1) * 100);
        return M_PROPERTY_OK;
    }

    return m_property_double_ro(action, arg, val);
}

static int mp_property_display_sync_active(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, mpctx->display_sync_active);
}

/// filename with path (RO)
static int mp_property_path(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filename)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, mpctx->filename);
}

static int mp_property_filename(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filename)
        return M_PROPERTY_UNAVAILABLE;
    char *filename = talloc_strdup(NULL, mpctx->filename);
    if (mp_is_url(bstr0(filename)))
        mp_url_unescape_inplace(filename);
    char *f = (char *)mp_basename(filename);
    if (!f[0])
        f = filename;
    if (action == M_PROPERTY_KEY_ACTION) {
        struct m_property_action_arg *ka = arg;
        if (strcmp(ka->key, "no-ext") == 0) {
            action = ka->action;
            arg = ka->arg;
            bstr root;
            if (mp_splitext(f, &root))
                f = bstrto0(filename, root);
        }
    }
    int r = m_property_strdup_ro(action, arg, f);
    talloc_free(filename);
    return r;
}

static int mp_property_stream_open_filename(void *ctx, struct m_property *prop,
                                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->stream_open_filename || !mpctx->playing)
        return M_PROPERTY_UNAVAILABLE;
    switch (action) {
    case M_PROPERTY_SET: {
        if (mpctx->demuxer)
            return M_PROPERTY_ERROR;
        mpctx->stream_open_filename =
            talloc_strdup(mpctx->stream_open_filename, *(char **)arg);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
    case M_PROPERTY_GET:
        return m_property_strdup_ro(action, arg, mpctx->stream_open_filename);
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_file_size(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    int64_t size;
    if (demux_stream_control(mpctx->demuxer, STREAM_CTRL_GET_SIZE, &size) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = format_file_size(size);
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, size);
}

static int mp_property_media_title(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    char *name = NULL;
    if (mpctx->opts->media_title)
        name = mpctx->opts->media_title;
    if (name && name[0])
        return m_property_strdup_ro(action, arg, name);
    if (mpctx->demuxer) {
        name = mp_tags_get_str(mpctx->demuxer->metadata, "service_name");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
        name = mp_tags_get_str(mpctx->demuxer->metadata, "title");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
        name = mp_tags_get_str(mpctx->demuxer->metadata, "icy-title");
        if (name && name[0])
            return m_property_strdup_ro(action, arg, name);
    }
    if (mpctx->playing && mpctx->playing->title)
        return m_property_strdup_ro(action, arg, mpctx->playing->title);
    return mp_property_filename(ctx, prop, action, arg);
}

static int mp_property_stream_path(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer || !mpctx->demuxer->filename)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, mpctx->demuxer->filename);
}

/// Demuxer name (RO)
static int mp_property_demuxer(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_strdup_ro(action, arg, demuxer->desc->name);
}

static int mp_property_file_format(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;
    const char *name = demuxer->filetype ? demuxer->filetype : demuxer->desc->name;
    return m_property_strdup_ro(action, arg, name);
}

static int mp_property_stream_pos(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer || demuxer->filepos < 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int64_ro(action, arg, demuxer->filepos);
}

/// Stream end offset (RO)
static int mp_property_stream_end(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    return mp_property_file_size(ctx, prop, action, arg);
}

// Does some magic to handle "<name>/full" as time formatted with milliseconds.
// Assumes prop is the type of the actual property.
static int property_time(int action, void *arg, double time)
{
    if (time == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;

    const struct m_option time_type = {.type = CONF_TYPE_TIME};
    switch (action) {
    case M_PROPERTY_GET:
        *(double *)arg = time;
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = time_type;
        return M_PROPERTY_OK;
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;

        if (strcmp(ka->key, "full") != 0)
            return M_PROPERTY_UNKNOWN;

        switch (ka->action) {
        case M_PROPERTY_GET:
            *(double *)ka->arg = time;
            return M_PROPERTY_OK;
        case M_PROPERTY_PRINT:
            *(char **)ka->arg = mp_format_time(time, true);
            return M_PROPERTY_OK;
        case M_PROPERTY_GET_TYPE:
            *(struct m_option *)ka->arg = time_type;
            return M_PROPERTY_OK;
        }
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_duration(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    double len = get_time_length(mpctx);

    if (len < 0)
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, len);
}

static int mp_property_avsync(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->ao_chain || !mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;
    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_asprintf(NULL, "%7.3f", mpctx->last_av_difference);
        return M_PROPERTY_OK;
    }
    return m_property_double_ro(action, arg, mpctx->last_av_difference);
}

static int mp_property_total_avsync_change(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->ao_chain || !mpctx->vo_chain)
        return M_PROPERTY_UNAVAILABLE;
    if (mpctx->total_avsync_change == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_double_ro(action, arg, mpctx->total_avsync_change);
}

static int mp_property_frame_drop_dec(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_decoder_wrapper *dec = mpctx->vo_chain && mpctx->vo_chain->track
        ? mpctx->vo_chain->track->dec : NULL;
    if (!dec)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, dec->dropped_frames);
}

static int mp_property_mistimed_frame_count(void *ctx, struct m_property *prop,
                                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->vo_chain || !mpctx->display_sync_active)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_int_ro(action, arg, mpctx->mistimed_frames_total);
}

/// Current position in percent (RW)
static int mp_property_percent_pos(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    switch (action) {
    case M_PROPERTY_SET: {
        double pos = *(double *)arg;
        queue_seek(mpctx, MPSEEK_FACTOR, pos / 100.0, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        double pos = get_current_pos_ratio(mpctx, false) * 100.0;
        if (pos < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(double *)arg = pos;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_DOUBLE,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = 100,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        int pos = get_percent_pos(mpctx);
        if (pos < 0)
            return M_PROPERTY_UNAVAILABLE;
        *(char **)arg = talloc_asprintf(NULL, "%d", pos);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_time_start(void *ctx, struct m_property *prop,
                                  int action, void *arg)
{
    // minor backwards-compat.
    return property_time(action, arg, 0);
}

/// Current position in seconds (RW)
static int mp_property_time_pos(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_SET) {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, *(double *)arg, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    return property_time(action, arg, get_current_time(mpctx));
}

/// Current audio pts in seconds (R)
static int mp_property_audio_pts(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized || mpctx->audio_status < STATUS_PLAYING ||
        mpctx->audio_status >= STATUS_EOF)
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, playing_audio_pts(mpctx));
}

static bool time_remaining(MPContext *mpctx, double *remaining)
{
    double len = get_time_length(mpctx);
    double playback = get_playback_time(mpctx);

    if (playback == MP_NOPTS_VALUE || len <= 0)
        return false;

    *remaining = len - playback;

    return len >= 0;
}

static int mp_property_remaining(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    double remaining;
    if (!time_remaining(ctx, &remaining))
        return M_PROPERTY_UNAVAILABLE;

    return property_time(action, arg, remaining);
}

static int mp_property_playtime_remaining(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    double remaining;
    if (!time_remaining(mpctx, &remaining))
        return M_PROPERTY_UNAVAILABLE;

    double speed = mpctx->video_speed;
    return property_time(action, arg, remaining / speed);
}

static int mp_property_playback_time(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_SET) {
        queue_seek(mpctx, MPSEEK_ABSOLUTE, *(double *)arg, MPSEEK_DEFAULT, 0);
        return M_PROPERTY_OK;
    }
    return property_time(action, arg, get_playback_time(mpctx));
}

static int get_tag_entry(int item, int action, void *arg, void *ctx)
{
    struct mp_tags *tags = ctx;

    struct m_sub_property props[] = {
        {"key",     SUB_PROP_STR(tags->keys[item])},
        {"value",   SUB_PROP_STR(tags->values[item])},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int tag_property(int action, void *arg, struct mp_tags *tags)
{
    switch (action) {
    case M_PROPERTY_GET: {
        mpv_node_list *list = talloc_zero(NULL, mpv_node_list);
        mpv_node node = {
            .format = MPV_FORMAT_NODE_MAP,
            .u.list = list,
        };
        list->num = tags->num_keys;
        list->values = talloc_array(list, mpv_node, list->num);
        list->keys = talloc_array(list, char*, list->num);
        for (int n = 0; n < tags->num_keys; n++) {
            list->keys[n] = talloc_strdup(list, tags->keys[n]);
            list->values[n] = (struct mpv_node){
                .format = MPV_FORMAT_STRING,
                .u.string = talloc_strdup(list, tags->values[n]),
            };
        }
        *(mpv_node*)arg = node;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE: {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_PRINT: {
        char *res = NULL;
        for (int n = 0; n < tags->num_keys; n++) {
            res = talloc_asprintf_append_buffer(res, "%s: %s\n",
                                                tags->keys[n], tags->values[n]);
        }
        if (!res)
            res = talloc_strdup(NULL, "(empty)");
        *(char **)arg = res;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        if (bstr_equals0(key, "list")) {
            struct m_property_action_arg nka = *ka;
            nka.key = rem;
            return m_property_read_list(action, &nka, tags->num_keys,
                                        get_tag_entry, tags);
        }
        // Direct access without this prefix is allowed for compatibility.
        bstr k = bstr0(ka->key);
        bstr_eatstart0(&k, "by-key/");
        char *meta = mp_tags_get_bstr(tags, k);
        if (!meta)
            return M_PROPERTY_UNKNOWN;
        switch (ka->action) {
        case M_PROPERTY_GET:
            *(char **)ka->arg = talloc_strdup(NULL, meta);
            return M_PROPERTY_OK;
        case M_PROPERTY_GET_TYPE:
            *(struct m_option *)ka->arg = (struct m_option){
                .type = CONF_TYPE_STRING,
            };
            return M_PROPERTY_OK;
        }
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

/// Demuxer meta data
static int mp_property_metadata(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct demuxer *demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return tag_property(action, arg, demuxer->metadata);
}

static int mp_property_filtered_metadata(void *ctx, struct m_property *prop,
                                         int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->filtered_tags)
        return M_PROPERTY_UNAVAILABLE;

    return tag_property(action, arg, mpctx->filtered_tags);
}

static int mp_property_chapter_metadata(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    int chapter = get_current_chapter(mpctx);
    if (chapter < 0)
        return M_PROPERTY_UNAVAILABLE;
    return tag_property(action, arg, mpctx->chapters[chapter].metadata);
}

static int mp_property_filter_metadata(void *ctx, struct m_property *prop,
                                       int action, void *arg)
{
    MPContext *mpctx = ctx;
    const char *type = prop->priv;

    if (action == M_PROPERTY_KEY_ACTION) {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        struct mp_tags *metadata = NULL;
        struct mp_output_chain *chain = NULL;
        if (strcmp(type, "af") == 0) {
            chain = mpctx->ao_chain ? mpctx->ao_chain->filter : NULL;
        }
        if (!chain)
            return M_PROPERTY_UNAVAILABLE;

        struct mp_filter_command cmd = {
            .type = MP_FILTER_COMMAND_GET_META,
            .res = &metadata,
        };
        mp_output_chain_command(chain, mp_tprintf(80, "%.*s", BSTR_P(key)), &cmd);

        if (!metadata)
            return M_PROPERTY_ERROR;

        int res;
        if (strlen(rem)) {
            struct m_property_action_arg next_ka = *ka;
            next_ka.key = rem;
            res = tag_property(M_PROPERTY_KEY_ACTION, &next_ka, metadata);
        } else {
            res = tag_property(ka->action, ka->arg, metadata);
        }
        talloc_free(metadata);
        return res;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_pause(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    MPContext *mpctx = ctx;

    if (mpctx->playback_initialized && action == M_PROPERTY_SET) {
        set_pause_state(mpctx, *(int *)arg);
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_core_idle(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !mpctx->playback_active);
}

static int mp_property_idle(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    return m_property_flag_ro(action, arg, cmd->is_idle);
}

static int mp_property_eof_reached(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    bool eof = mpctx->video_status == STATUS_EOF &&
               mpctx->audio_status == STATUS_EOF;
    return m_property_flag_ro(action, arg, eof);
}

static int mp_property_seeking(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, !mpctx->restart_complete);
}

static int mp_property_playback_abort(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !mpctx->playing || mpctx->stop_play);
}

static int mp_property_cache_speed(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    uint64_t val = s.bytes_per_second;

    if (action == M_PROPERTY_PRINT) {
        *(char **)arg = talloc_strdup_append(format_file_size(val), "/s");
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, val);
}

static int mp_property_demuxer_cache_duration(void *ctx, struct m_property *prop,
                                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (s.ts_duration < 0)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, s.ts_duration);
}

static int mp_property_demuxer_cache_time(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    if (s.ts_end == MP_NOPTS_VALUE)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, s.ts_end);
}

static int mp_property_demuxer_cache_idle(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_flag_ro(action, arg, s.idle);
}

static int mp_property_demuxer_cache_state(void *ctx, struct m_property *prop,
                                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    if (action == M_PROPERTY_GET_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    }
    if (action != M_PROPERTY_GET)
        return M_PROPERTY_NOT_IMPLEMENTED;

    struct demux_ctrl_reader_state s;
    if (demux_control(mpctx->demuxer, DEMUXER_CTRL_GET_READER_STATE, &s) < 1)
        return M_PROPERTY_UNAVAILABLE;

    struct mpv_node *r = (struct mpv_node *)arg;
    node_init(r, MPV_FORMAT_NODE_MAP, NULL);

    struct mpv_node *ranges =
        node_map_add(r, "seekable-ranges", MPV_FORMAT_NODE_ARRAY);
    for (int n = 0; n < s.num_seek_ranges; n++) {
        struct demux_seek_range *range = &s.seek_ranges[n];
        struct mpv_node *sub = node_array_add(ranges, MPV_FORMAT_NODE_MAP);
        node_map_add_double(sub, "start", range->start);
        node_map_add_double(sub, "end", range->end);
    }

    if (s.ts_end != MP_NOPTS_VALUE)
        node_map_add_double(r, "cache-end", s.ts_end);

    if (s.ts_reader != MP_NOPTS_VALUE)
        node_map_add_double(r, "reader-pts", s.ts_reader);

    node_map_add_flag(r, "eof", s.eof);
    node_map_add_flag(r, "underrun", s.underrun);
    node_map_add_flag(r, "idle", s.idle);
    node_map_add_int64(r, "total-bytes", s.total_bytes);
    node_map_add_int64(r, "fw-bytes", s.fw_bytes);
    if (s.seeking != MP_NOPTS_VALUE)
        node_map_add_double(r, "debug-seeking", s.seeking);
    node_map_add_int64(r, "debug-low-level-seeks", s.low_level_seeks);
    if (s.ts_last != MP_NOPTS_VALUE)
        node_map_add_double(r, "debug-ts-last", s.ts_last);

    return M_PROPERTY_OK;
}

static int mp_property_demuxer_start_time(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_double_ro(action, arg, mpctx->demuxer->start_time);
}

static int mp_property_paused_for_cache(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->playback_initialized)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->paused_for_cache);
}

static int mp_property_cache_buffering(void *ctx, struct m_property *prop,
                                       int action, void *arg)
{
    MPContext *mpctx = ctx;
    int state = get_cache_buffering_percentage(mpctx);
    if (state < 0)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_int_ro(action, arg, state);
}

static int mp_property_demuxer_is_network(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;

    return m_property_flag_ro(action, arg, mpctx->demuxer->is_network);
}


static int mp_property_clock(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    char outstr[6];
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);

    if ((tmp != NULL) && (strftime(outstr, sizeof(outstr), "%H:%M", tmp) == 5))
        return m_property_strdup_ro(action, arg, outstr);
    return M_PROPERTY_UNAVAILABLE;
}

static int mp_property_seekable(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->demuxer->seekable);
}

static int mp_property_partially_seekable(void *ctx, struct m_property *prop,
                                          int action, void *arg)
{
    MPContext *mpctx = ctx;
    if (!mpctx->demuxer)
        return M_PROPERTY_UNAVAILABLE;
    return m_property_flag_ro(action, arg, mpctx->demuxer->partially_seekable);
}

static int mp_property_mixer_active(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_flag_ro(action, arg, !!mpctx->ao);
}

/// Volume (RW)
static int mp_property_volume(void *ctx, struct m_property *prop,
                              int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;

    switch (action) {
    case M_PROPERTY_GET_CONSTRICTED_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_FLOAT,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = opts->softvol_max,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT:
        *(char **)arg = talloc_asprintf(NULL, "%i", (int)opts->softvol_volume);
        return M_PROPERTY_OK;
    }

    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Mute (RW)
static int mp_property_mute(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    MPContext *mpctx = ctx;

    if (action == M_PROPERTY_GET_CONSTRICTED_TYPE) {
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }

    int r = mp_property_generic_option(mpctx, prop, action, arg);
    if (action == M_PROPERTY_SET)
        audio_update_volume(mpctx);
    return r;
}

static int mp_property_ao_volume(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct ao *ao = mpctx->ao;
    if (!ao)
        return M_PROPERTY_NOT_IMPLEMENTED;

    switch (action) {
    case M_PROPERTY_SET: {
        float value = *(float *)arg;
        ao_control_vol_t vol = {value, value};
        if (ao_control(ao, AOCONTROL_SET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        ao_control_vol_t vol = {0};
        if (ao_control(ao, AOCONTROL_GET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(float *)arg = (vol.left + vol.right) / 2.0f;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){
            .type = CONF_TYPE_FLOAT,
            .flags = M_OPT_RANGE,
            .min = 0,
            .max = 100,
        };
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT: {
        ao_control_vol_t vol = {0};
        if (ao_control(ao, AOCONTROL_GET_VOLUME, &vol) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(char **)arg = talloc_asprintf(NULL, "%.f", (vol.left + vol.right) / 2.0f);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}


static int mp_property_ao_mute(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct ao *ao = mpctx->ao;
    if (!ao)
        return M_PROPERTY_NOT_IMPLEMENTED;

    switch (action) {
    case M_PROPERTY_SET: {
        bool value = *(int *)arg;
        if (ao_control(ao, AOCONTROL_SET_MUTE, &value) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET: {
        bool value = false;
        if (ao_control(ao, AOCONTROL_GET_MUTE, &value) != CONTROL_OK)
            return M_PROPERTY_UNAVAILABLE;
        *(int *)arg = value;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_FLAG};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_device_entry(int item, int action, void *arg, void *ctx)
{
    struct ao_device_list *list = ctx;
    struct ao_device_desc *entry = &list->devices[item];

    struct m_sub_property props[] = {
        {"name",        SUB_PROP_STR(entry->name)},
        {"description", SUB_PROP_STR(entry->desc)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static void create_hotplug(struct MPContext *mpctx)
{
    struct command_ctx *cmd = mpctx->command_ctx;

    if (!cmd->hotplug) {
        cmd->hotplug = ao_hotplug_create(mpctx->global, mp_wakeup_core_cb,
                                         mpctx);
    }
}

static int mp_property_audio_device(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    if (action == M_PROPERTY_PRINT) {
        create_hotplug(mpctx);

        char *name = NULL;
        if (mp_property_generic_option(mpctx, prop, M_PROPERTY_GET, &name) < 1)
            name = NULL;

        struct ao_device_list *list = ao_hotplug_get_device_list(cmd->hotplug);
        for (int n = 0; n < list->num_devices; n++) {
            struct ao_device_desc *dev = &list->devices[n];
            if (dev->name && name && strcmp(dev->name, name) == 0) {
                *(char **)arg = talloc_strdup(NULL, dev->desc ? dev->desc : "?");
                talloc_free(name);
                return M_PROPERTY_OK;
            }
        }

        talloc_free(name);
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_audio_devices(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    create_hotplug(mpctx);

    struct ao_device_list *list = ao_hotplug_get_device_list(cmd->hotplug);
    return m_property_read_list(action, arg, list->num_devices,
                                get_device_entry, list);
}

static int mp_property_ao(void *ctx, struct m_property *p, int action, void *arg)
{
    MPContext *mpctx = ctx;
    return m_property_strdup_ro(action, arg,
                                    mpctx->ao ? ao_get_name(mpctx->ao) : NULL);
}

/// Audio delay (RW)
static int mp_property_audio_delay(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    float delay = mpctx->opts->audio_delay;
    switch (action) {
    case M_PROPERTY_PRINT:
        *(char **)arg = format_delay(delay);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET: {
        int r = mp_property_generic_option(mpctx, prop, action, arg);
        if (mpctx->ao_chain && mpctx->vo_chain)
            mpctx->delay += mpctx->opts->audio_delay - delay;
        mp_wakeup_core(mpctx);
        return r;
    }
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Audio codec tag (RO)
static int mp_property_audio_codec_name(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_AUDIO];
    const char *c = track && track->stream ? track->stream->codec->codec : NULL;
    return m_property_strdup_ro(action, arg, c);
}

/// Audio codec name (RO)
static int mp_property_audio_codec(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct track *track = mpctx->current_track[0][STREAM_AUDIO];
    const char *c = track && track->dec ? track->dec->decoder_desc : NULL;
    return m_property_strdup_ro(action, arg, c);
}

static int property_audiofmt(struct mp_aframe *fmt, int action, void *arg)
{
    if (!fmt || !mp_aframe_config_is_valid(fmt))
        return M_PROPERTY_UNAVAILABLE;

    struct mp_chmap chmap = {0};
    mp_aframe_get_chmap(fmt, &chmap);

    struct m_sub_property props[] = {
        {"samplerate",      SUB_PROP_INT(mp_aframe_get_rate(fmt))},
        {"channel-count",   SUB_PROP_INT(chmap.num)},
        {"channels",        SUB_PROP_STR(mp_chmap_to_str(&chmap))},
        {"hr-channels",     SUB_PROP_STR(mp_chmap_to_str_hr(&chmap))},
        {"format",          SUB_PROP_STR(af_fmt_to_str(mp_aframe_get_format(fmt)))},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_audio_params(void *ctx, struct m_property *prop,
                                    int action, void *arg)
{
    MPContext *mpctx = ctx;
    return property_audiofmt(mpctx->ao_chain ?
        mpctx->ao_chain->filter->input_aformat : NULL, action, arg);
}

static int mp_property_audio_out_params(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct mp_aframe *frame = NULL;
    if (mpctx->ao) {
        frame = mp_aframe_create();
        int samplerate;
        int format;
        struct mp_chmap channels;
        ao_get_format(mpctx->ao, &samplerate, &format, &channels);
        mp_aframe_set_rate(frame, samplerate);
        mp_aframe_set_format(frame, format);
        mp_aframe_set_chmap(frame, &channels);
    }
    int r = property_audiofmt(frame, action, arg);
    talloc_free(frame);
    return r;
}

static struct track* track_next(struct MPContext *mpctx, enum stream_type type,
                                int direction, struct track *track)
{
    assert(direction == -1 || direction == +1);
    struct track *prev = NULL, *next = NULL;
    bool seen = track == NULL;
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *cur = mpctx->tracks[n];
        if (cur->type == type) {
            if (cur == track) {
                seen = true;
            } else if (!cur->selected) {
                if (seen && !next) {
                    next = cur;
                }
                if (!seen || !track) {
                    prev = cur;
                }
            }
        }
    }
    return direction > 0 ? next : prev;
}

static int property_switch_track(struct m_property *prop, int action, void *arg,
                                 MPContext *mpctx, int order,
                                 enum stream_type type)
{
    struct track *track = mpctx->current_track[order][type];

    switch (action) {
    case M_PROPERTY_GET:
        if (mpctx->playback_initialized) {
            *(int *)arg = track ? track->user_tid : -2;
        } else {
            *(int *)arg = mpctx->opts->stream_id[order][type];
        }
        return M_PROPERTY_OK;
    case M_PROPERTY_PRINT:
        if (track) {
            char *lang = track->lang;
            if (!lang)
                lang = "unknown";

            if (track->title) {
                *(char **)arg = talloc_asprintf(NULL, "(%d) %s (\"%s\")",
                                           track->user_tid, lang, track->title);
            } else {
                *(char **)arg = talloc_asprintf(NULL, "(%d) %s",
                                                track->user_tid, lang);
            }
        } else {
            const char *msg = "no";
            if (!mpctx->playback_initialized &&
                mpctx->opts->stream_id[order][type] == -1)
                msg = "auto";
            *(char **) arg = talloc_strdup(NULL, msg);
        }
        return M_PROPERTY_OK;

    case M_PROPERTY_SWITCH: {
        if (mpctx->playback_initialized) {
            struct m_property_switch_arg *sarg = arg;
            do {
                track = track_next(mpctx, type, sarg->inc >= 0 ? +1 : -1, track);
                mp_switch_track_n(mpctx, order, type, track, FLAG_MARK_SELECTION);
            } while (mpctx->current_track[order][type] != track);
            print_track_list(mpctx, "Track switched:");
        } else {
            // Simply cycle between "no" and "auto". It's possible that this does
            // not always do what the user means, but keep the complexity low.
            mpctx->opts->stream_id[order][type] =
                mpctx->opts->stream_id[order][type] == -1 ? -2 : -1;
        }
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET:
        if (mpctx->playback_initialized) {
            track = mp_track_by_tid(mpctx, type, *(int *)arg);
            mp_switch_track_n(mpctx, order, type, track, FLAG_MARK_SELECTION);
            print_track_list(mpctx, "Track switched:");
            mp_wakeup_core(mpctx);
        } else {
            mpctx->opts->stream_id[order][type] = *(int *)arg;
        }
        return M_PROPERTY_OK;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

/// Selected audio id (RW)
static int mp_property_audio(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    return property_switch_track(prop, action, arg, ctx, 0, STREAM_AUDIO);
}

static char *print_obj_osd_list(struct m_obj_settings *list)
{
    char *res = NULL;
    for (int n = 0; list && list[n].name; n++) {
        res = talloc_asprintf_append(res, "%s [", list[n].name);
        for (int i = 0; list[n].attribs && list[n].attribs[i]; i += 2) {
            res = talloc_asprintf_append(res, "%s%s=%s", i > 0 ? " " : "",
                                         list[n].attribs[i],
                                         list[n].attribs[i + 1]);
        }
        res = talloc_asprintf_append(res, "]");
        if (!list[n].enabled)
            res = talloc_strdup_append(res, " (disabled)");
        res = talloc_strdup_append(res, "\n");
    }
    if (!res)
        res = talloc_strdup(NULL, "(empty)");
    return res;
}

static int property_filter(struct m_property *prop, int action, void *arg,
                           MPContext *mpctx, enum stream_type mt)
{
    switch (action) {
    case M_PROPERTY_PRINT: {
        struct m_config_option *opt = m_config_get_co(mpctx->mconfig,
                                                      bstr0(prop->name));
        *(char **)arg = print_obj_osd_list(*(struct m_obj_settings **)opt->data);
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_SET:
        return set_filters(mpctx, mt, *(struct m_obj_settings **)arg) >= 0
            ? M_PROPERTY_OK : M_PROPERTY_ERROR;
    }
    return mp_property_generic_option(mpctx, prop, action, arg);
}

static int mp_property_af(void *ctx, struct m_property *prop,
                          int action, void *arg)
{
    return property_filter(prop, action, arg, ctx, STREAM_AUDIO);
}

static int mp_property_ab_loop(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct MPOpts *opts = mpctx->opts;
    if (action == M_PROPERTY_KEY_ACTION) {
        double val;
        if (mp_property_generic_option(mpctx, prop, M_PROPERTY_GET, &val) < 1)
            return M_PROPERTY_ERROR;

        return property_time(action, arg, val);
    }
    int r = mp_property_generic_option(mpctx, prop, action, arg);
    if (r > 0 && action == M_PROPERTY_SET) {
        mpctx->ab_loop_clip = mpctx->playback_pts < opts->ab_loop[1];
        if (strcmp(prop->name, "ab-loop-b") == 0) {
            if (opts->ab_loop[1] != MP_NOPTS_VALUE &&
                mpctx->playback_pts <= opts->ab_loop[1])
                mpctx->ab_loop_clip = true;
        }
        // Update if visible

        // HISONA ...
        printf("%s: OSD_BAR_SEEK \n", __func__);

        mp_wakeup_core(mpctx);
    }
    return r;
}

static int mp_property_packet_bitrate(void *ctx, struct m_property *prop,
                                      int action, void *arg)
{
    MPContext *mpctx = ctx;
    int type = (uintptr_t)prop->priv & ~0x100;
    bool old = (uintptr_t)prop->priv & 0x100;

    struct demuxer *demuxer = NULL;
    if (mpctx->current_track[0][type])
        demuxer = mpctx->current_track[0][type]->demuxer;
    if (!demuxer)
        demuxer = mpctx->demuxer;
    if (!demuxer)
        return M_PROPERTY_UNAVAILABLE;

    double r[STREAM_TYPE_COUNT];
    if (demux_control(demuxer, DEMUXER_CTRL_GET_BITRATE_STATS, &r) < 1)
        return M_PROPERTY_UNAVAILABLE;
    if (r[type] < 0)
        return M_PROPERTY_UNAVAILABLE;

    // r[type] is in bytes/second -> bits
    double rate = r[type] * 8;

    // Same story, but used kilobits for some reason.
    if (old)
        return m_property_int64_ro(action, arg, rate / 1000.0 + 0.5);

    if (action == M_PROPERTY_PRINT) {
        rate /= 1000;
        if (rate < 1000) {
            *(char **)arg = talloc_asprintf(NULL, "%d kbps", (int)rate);
        } else {
            *(char **)arg = talloc_asprintf(NULL, "%.3f mbps", rate / 1000.0);
        }
        return M_PROPERTY_OK;
    }
    return m_property_int64_ro(action, arg, rate);
}

static int mp_property_cwd(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET: {
        char *cwd = mp_getcwd(NULL);
        if (!cwd)
            return M_PROPERTY_ERROR;
        *(char **)arg = cwd;
        return M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_protocols(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(char ***)arg = stream_get_proto_list();
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int get_decoder_entry(int item, int action, void *arg, void *ctx)
{
    struct mp_decoder_list *codecs = ctx;
    struct mp_decoder_entry *c = &codecs->entries[item];

    struct m_sub_property props[] = {
        {"codec",       SUB_PROP_STR(c->codec)},
        {"driver" ,     SUB_PROP_STR(c->decoder)},
        {"description", SUB_PROP_STR(c->desc)},
        {0}
    };

    return m_property_read_sub(props, action, arg);
}

static int mp_property_decoders(void *ctx, struct m_property *prop,
                                int action, void *arg)
{
    struct mp_decoder_list *codecs = talloc_zero(NULL, struct mp_decoder_list);
    struct mp_decoder_list *a = talloc_steal(codecs, audio_decoder_list());
    mp_append_decoders(codecs, a);
    int r = m_property_read_list(action, arg, codecs->num_entries,
                                 get_decoder_entry, codecs);
    talloc_free(codecs);
    return r;
}

static int mp_property_lavf_demuxers(void *ctx, struct m_property *prop,
                                 int action, void *arg)
{
    switch (action) {
    case M_PROPERTY_GET:
        *(char ***)arg = mp_get_lavf_demuxers();
        return M_PROPERTY_OK;
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_version(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    return m_property_strdup_ro(action, arg, mpa_version);
}

static int mp_property_configuration(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    return m_property_strdup_ro(action, arg, CONFIGURATION);
}

static int mp_property_ffmpeg(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    return m_property_strdup_ro(action, arg, av_version_info());
}

static int mp_property_alias(void *ctx, struct m_property *prop,
                             int action, void *arg)
{
    const char *real_property = prop->priv;
    return mp_property_do(real_property, action, arg, ctx);
}

static int mp_property_deprecated_alias(void *ctx, struct m_property *prop,
                                        int action, void *arg)
{
    MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;
    const char *real_property = prop->priv;
    for (int n = 0; n < cmd->num_warned_deprecated; n++) {
        if (strcmp(cmd->warned_deprecated[n], prop->name) == 0)
            goto done;
    }
    MP_WARN(mpctx, "Warning: property '%s' was replaced with '%s' and "
            "might be removed in the future.\n", prop->name, real_property);
    MP_TARRAY_APPEND(cmd, cmd->warned_deprecated, cmd->num_warned_deprecated,
                     (char *)prop->name);

done:
    return mp_property_do(real_property, action, arg, ctx);
}

static int access_options(struct m_property_action_arg *ka, bool local,
                          MPContext *mpctx)
{
    struct m_config_option *opt = m_config_get_co(mpctx->mconfig,
                                                  bstr0(ka->key));
    if (!opt)
        return M_PROPERTY_UNKNOWN;
    if (!opt->data)
        return M_PROPERTY_UNAVAILABLE;

    switch (ka->action) {
    case M_PROPERTY_GET:
        m_option_copy(opt->opt, ka->arg, opt->data);
        return M_PROPERTY_OK;
    case M_PROPERTY_SET: {
        if (local && !mpctx->playing)
            return M_PROPERTY_ERROR;
        int flags = M_SETOPT_RUNTIME | (local ? M_SETOPT_BACKUP : 0);
        int r = m_config_set_option_raw(mpctx->mconfig, opt, ka->arg, flags);
        mp_wakeup_core(mpctx);
        return r < 0 ? M_PROPERTY_ERROR : M_PROPERTY_OK;
    }
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)ka->arg = *opt->opt;
        return M_PROPERTY_OK;
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int access_option_list(int action, void *arg, bool local, MPContext *mpctx)
{
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET:
        *(char ***)arg = m_config_list_options(NULL, mpctx->mconfig);
        return M_PROPERTY_OK;
    case M_PROPERTY_KEY_ACTION:
        return access_options(arg, local, mpctx);
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}


static int mp_property_options(void *ctx, struct m_property *prop,
                               int action, void *arg)
{
    MPContext *mpctx = ctx;
    return access_option_list(action, arg, false, mpctx);
}

static int mp_property_local_options(void *ctx, struct m_property *prop,
                                     int action, void *arg)
{
    MPContext *mpctx = ctx;
    return access_option_list(action, arg, true, mpctx);
}

static int mp_property_option_info(void *ctx, struct m_property *prop,
                                   int action, void *arg)
{
    MPContext *mpctx = ctx;
    switch (action) {
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *ka = arg;
        bstr key;
        char *rem;
        m_property_split_path(ka->key, &key, &rem);
        struct m_config_option *co = m_config_get_co(mpctx->mconfig, key);
        if (!co)
            return M_PROPERTY_UNKNOWN;
        const struct m_option *opt = co->opt;

        union m_option_value def = {0};
        const void *def_ptr = m_config_get_co_default(mpctx->mconfig, co);
        if (def_ptr && opt->type->size > 0)
            memcpy(&def, def_ptr, opt->type->size);

        bool has_minmax =
            opt->type == &m_option_type_int ||
            opt->type == &m_option_type_int64 ||
            opt->type == &m_option_type_float ||
            opt->type == &m_option_type_double;
        char **choices = NULL;

        if (opt->type == &m_option_type_choice) {
            has_minmax = true;
            struct m_opt_choice_alternatives *alt = opt->priv;
            int num = 0;
            for ( ; alt->name; alt++)
                MP_TARRAY_APPEND(NULL, choices, num, alt->name);
            MP_TARRAY_APPEND(NULL, choices, num, NULL);
        }
        if (opt->type == &m_option_type_obj_settings_list) {
            struct m_obj_list *objs = opt->priv;
            int num = 0;
            for (int n = 0; ; n++) {
                struct m_obj_desc desc = {0};
                if (!objs->get_desc(&desc, n))
                    break;
                MP_TARRAY_APPEND(NULL, choices, num, (char *)desc.name);
            }
            MP_TARRAY_APPEND(NULL, choices, num, NULL);
        }

        struct m_sub_property props[] = {
            {"name",                    SUB_PROP_STR(co->name)},
            {"type",                    SUB_PROP_STR(opt->type->name)},
            {"set-from-commandline",    SUB_PROP_FLAG(co->is_set_from_cmdline)},
            {"set-locally",             SUB_PROP_FLAG(co->is_set_locally)},
            {"default-value",           *opt, def},
            {"min",                     SUB_PROP_DOUBLE(opt->min),
             .unavailable = !(has_minmax && (opt->flags & M_OPT_MIN))},
            {"max",                     SUB_PROP_DOUBLE(opt->max),
             .unavailable = !(has_minmax && (opt->flags & M_OPT_MAX))},
            {"choices", .type = {.type = CONF_TYPE_STRING_LIST},
             .value = {.string_list = choices}, .unavailable = !choices},
            {0}
        };

        struct m_property_action_arg next_ka = *ka;
        next_ka.key = rem;
        int r = m_property_read_sub(props, M_PROPERTY_KEY_ACTION, &next_ka);
        talloc_free(choices);
        return r;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_property_list(void *ctx, struct m_property *prop,
                            int action, void *arg)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;

    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_STRING_LIST};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET: {
        char **list = NULL;
        int num = 0;
        for (int n = 0; cmd->properties[n].name; n++) {
            MP_TARRAY_APPEND(NULL, list, num,
                                talloc_strdup(NULL, cmd->properties[n].name));
        }
        MP_TARRAY_APPEND(NULL, list, num, NULL);
        *(char ***)arg = list;
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

static int mp_profile_list(void *ctx, struct m_property *prop,
                           int action, void *arg)
{
    MPContext *mpctx = ctx;
    switch (action) {
    case M_PROPERTY_GET_TYPE:
        *(struct m_option *)arg = (struct m_option){.type = CONF_TYPE_NODE};
        return M_PROPERTY_OK;
    case M_PROPERTY_GET: {
        *(struct mpv_node *)arg = m_config_get_profiles(mpctx->mconfig);
        return M_PROPERTY_OK;
    }
    }
    return M_PROPERTY_NOT_IMPLEMENTED;
}

// Redirect a property name to another
#define M_PROPERTY_ALIAS(name, real_property) \
    {(name), mp_property_alias, .priv = (real_property)}

#define M_PROPERTY_DEPRECATED_ALIAS(name, real_property) \
    {(name), mp_property_deprecated_alias, .priv = (real_property)}

// Base list of properties. This does not include option-mapped properties.
static const struct m_property mp_properties_base[] = {
    // General
    {"speed", mp_property_playback_speed},
    {"audio-speed-correction", mp_property_av_speed_correction, .priv = "a"},
    {"video-speed-correction", mp_property_av_speed_correction, .priv = "v"},
    {"display-sync-active", mp_property_display_sync_active},
    {"filename", mp_property_filename},
    {"stream-open-filename", mp_property_stream_open_filename},
    {"file-size", mp_property_file_size},
    {"path", mp_property_path},
    {"media-title", mp_property_media_title},
    {"stream-path", mp_property_stream_path},
    {"current-demuxer", mp_property_demuxer},
    {"file-format", mp_property_file_format},
    {"stream-pos", mp_property_stream_pos},
    {"stream-end", mp_property_stream_end},
    {"duration", mp_property_duration},
    {"avsync", mp_property_avsync},
    {"total-avsync-change", mp_property_total_avsync_change},
    {"mistimed-frame-count", mp_property_mistimed_frame_count},
    {"decoder-frame-drop-count", mp_property_frame_drop_dec},
    {"percent-pos", mp_property_percent_pos},
    {"time-start", mp_property_time_start},
    {"time-pos", mp_property_time_pos},
    {"time-remaining", mp_property_remaining},
    {"audio-pts", mp_property_audio_pts},
    {"playtime-remaining", mp_property_playtime_remaining},
    {"playback-time", mp_property_playback_time},
    {"metadata", mp_property_metadata},
    {"filtered-metadata", mp_property_filtered_metadata},
    {"chapter-metadata", mp_property_chapter_metadata},
    {"af-metadata", mp_property_filter_metadata, .priv = "af"},
    {"pause", mp_property_pause},
    {"core-idle", mp_property_core_idle},
    {"eof-reached", mp_property_eof_reached},
    {"seeking", mp_property_seeking},
    {"playback-abort", mp_property_playback_abort},
    {"cache-speed", mp_property_cache_speed},
    {"demuxer-cache-duration", mp_property_demuxer_cache_duration},
    {"demuxer-cache-time", mp_property_demuxer_cache_time},
    {"demuxer-cache-idle", mp_property_demuxer_cache_idle},
    {"demuxer-start-time", mp_property_demuxer_start_time},
    {"demuxer-cache-state", mp_property_demuxer_cache_state},
    {"cache-buffering-state", mp_property_cache_buffering},
    {"paused-for-cache", mp_property_paused_for_cache},
    {"demuxer-via-network", mp_property_demuxer_is_network},
    {"clock", mp_property_clock},
    {"seekable", mp_property_seekable},
    {"partially-seekable", mp_property_partially_seekable},
    {"idle-active", mp_property_idle},

    M_PROPERTY_ALIAS("playlist-count", "playlist/count"),

    // Audio
    {"mixer-active", mp_property_mixer_active},
    {"volume", mp_property_volume},
    {"mute", mp_property_mute},
    {"ao-volume", mp_property_ao_volume},
    {"ao-mute", mp_property_ao_mute},
    {"audio-delay", mp_property_audio_delay},
    {"audio-codec-name", mp_property_audio_codec_name},
    {"audio-codec", mp_property_audio_codec},
    {"audio-params", mp_property_audio_params},
    {"audio-out-params", mp_property_audio_out_params},
    {"aid", mp_property_audio},
    {"audio-device", mp_property_audio_device},
    {"audio-device-list", mp_property_audio_devices},
    {"current-ao", mp_property_ao},

    {"af", mp_property_af},

    {"ab-loop-a", mp_property_ab_loop},
    {"ab-loop-b", mp_property_ab_loop},

#define PROPERTY_BITRATE(name, old, type) \
    {name, mp_property_packet_bitrate, (void *)(uintptr_t)((type)|(old?0x100:0))}
    PROPERTY_BITRATE("packet-video-bitrate", true, STREAM_VIDEO),
    PROPERTY_BITRATE("packet-audio-bitrate", true, STREAM_AUDIO),
    PROPERTY_BITRATE("packet-sub-bitrate", true, STREAM_SUB),

    PROPERTY_BITRATE("video-bitrate", false, STREAM_VIDEO),
    PROPERTY_BITRATE("audio-bitrate", false, STREAM_AUDIO),
    PROPERTY_BITRATE("sub-bitrate", false, STREAM_SUB),

    {"working-directory", mp_property_cwd},

    {"protocol-list", mp_property_protocols},
    {"decoder-list", mp_property_decoders},
    {"demuxer-lavf-list", mp_property_lavf_demuxers},

    {"mpv-version", mp_property_version},
    {"mpv-configuration", mp_property_configuration},
    {"ffmpeg-version", mp_property_ffmpeg},

    {"options", mp_property_options},
    {"file-local-options", mp_property_local_options},
    {"option-info", mp_property_option_info},
    {"property-list", mp_property_list},
    {"profile-list", mp_profile_list},

    M_PROPERTY_ALIAS("audio", "aid"),

    M_PROPERTY_DEPRECATED_ALIAS("drop-frame-count", "decoder-frame-drop-count"),
    M_PROPERTY_DEPRECATED_ALIAS("vo-drop-frame-count", "frame-drop-count"),
};

// Each entry describes which properties an event (possibly) changes.
#define E(x, ...) [x] = (const char*const[]){__VA_ARGS__, NULL}
static const char *const *const mp_event_property_change[] = {
    E(MPV_EVENT_START_FILE, "*"),
    E(MPV_EVENT_END_FILE, "*"),
    E(MPV_EVENT_FILE_LOADED, "*"),
    E(MP_EVENT_CHANGE_ALL, "*"),
    E(MPV_EVENT_TRACKS_CHANGED, "track-list"),
    E(MPV_EVENT_TRACK_SWITCHED, "vid", "video", "aid", "audio", "sid", "sub",
      "secondary-sid"),
    E(MPV_EVENT_IDLE, "*"),
    E(MPV_EVENT_PAUSE,   "pause"),
    E(MPV_EVENT_UNPAUSE, "pause"),
    E(MPV_EVENT_TICK, "time-pos", "audio-pts", "stream-pos", "avsync",
      "percent-pos", "time-remaining", "playtime-remaining", "playback-time",
      "estimated-vf-fps", "drop-frame-count", "vo-drop-frame-count",
      "total-avsync-change", "audio-speed-correction", "video-speed-correction",
      "vo-delayed-frame-count", "mistimed-frame-count", "vsync-ratio",
      "estimated-display-fps", "vsync-jitter", "sub-text", "audio-bitrate",
      "video-bitrate", "sub-bitrate", "decoder-frame-drop-count",
      "frame-drop-count", "video-frame-info"),
    E(MP_EVENT_DURATION_UPDATE, "duration"),
    E(MPV_EVENT_VIDEO_RECONFIG, "video-out-params", "video-params",
      "video-format", "video-codec", "video-bitrate", "dwidth", "dheight",
      "width", "height", "fps", "aspect", "vo-configured", "current-vo",
      "colormatrix", "colormatrix-input-range", "colormatrix-output-range",
      "colormatrix-primaries", "video-aspect", "video-dec-params",
      "hwdec", "hwdec-current", "hwdec-interop"),
    E(MPV_EVENT_AUDIO_RECONFIG, "audio-format", "audio-codec", "audio-bitrate",
      "samplerate", "channels", "audio", "volume", "mute",
      "current-ao", "audio-codec-name", "audio-params",
      "audio-out-params", "volume-max", "mixer-active"),
    E(MPV_EVENT_SEEK, "seeking", "core-idle", "eof-reached"),
    E(MPV_EVENT_PLAYBACK_RESTART, "seeking", "core-idle", "eof-reached"),
    E(MPV_EVENT_METADATA_UPDATE, "metadata", "filtered-metadata", "media-title"),
    E(MPV_EVENT_CHAPTER_CHANGE, "chapter", "chapter-metadata"),
    E(MP_EVENT_CACHE_UPDATE, "cache", "cache-free", "cache-used", "cache-idle",
      "demuxer-cache-duration", "demuxer-cache-idle", "paused-for-cache",
      "demuxer-cache-time", "cache-buffering-state", "cache-speed",
      "cache-percent"),
    E(MP_EVENT_WIN_RESIZE, "window-scale", "osd-width", "osd-height", "osd-par"),
    E(MP_EVENT_WIN_STATE, "window-minimized", "display-names", "display-fps",
      "fullscreen"),
    E(MP_EVENT_CHANGE_PLAYLIST, "playlist", "playlist-pos", "playlist-pos-1",
      "playlist-count", "playlist/count"),
    E(MP_EVENT_CORE_IDLE, "core-idle", "eof-reached"),
};
#undef E

// If there is no prefix, return length+1 (avoids matching full name as prefix).
static int prefix_len(const char *p)
{
    const char *end = strchr(p, '/');
    return end ? end - p : strlen(p) + 1;
}

static bool match_property(const char *a, const char *b)
{
    if (strcmp(a, "*") == 0)
        return true;
    // Give options and properties the same ID each, so change notifications
    // work both way.
    if (strncmp(a, "options/", 8) == 0)
        a += 8;
    if (strncmp(b, "options/", 8) == 0)
        b += 8;
    int len_a = prefix_len(a);
    int len_b = prefix_len(b);
    return strncmp(a, b, MPMIN(len_a, len_b)) == 0;
}

// Return a bitset of events which change the property.
uint64_t mp_get_property_event_mask(const char *name)
{
    uint64_t mask = 0;
    for (int n = 0; n < MP_ARRAY_SIZE(mp_event_property_change); n++) {
        const char *const *const list = mp_event_property_change[n];
        for (int i = 0; list && list[i]; i++) {
            if (match_property(list[i], name))
                mask |= 1ULL << n;
        }
    }
    return mask;
}

// Return an ID for the property. It might not be unique, but is good enough
// for property change handling. Return -1 if property unknown.
int mp_get_property_id(struct MPContext *mpctx, const char *name)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    for (int n = 0; ctx->properties[n].name; n++) {
        if (match_property(ctx->properties[n].name, name))
            return n;
    }
    return -1;
}

static bool is_property_set(int action, void *val)
{
    switch (action) {
    case M_PROPERTY_SET:
    case M_PROPERTY_SWITCH:
    case M_PROPERTY_SET_STRING:
    case M_PROPERTY_SET_NODE:
    case M_PROPERTY_MULTIPLY:
        return true;
    case M_PROPERTY_KEY_ACTION: {
        struct m_property_action_arg *key = val;
        return is_property_set(key->action, key->arg);
    }
    default:
        return false;
    }
}

static int mp_property_do_silent(const char *name, int action, void *val,
                                 struct MPContext *ctx)
{
    struct command_ctx *cmd = ctx->command_ctx;
    cmd->silence_option_deprecations += 1;
    int r = m_property_do(ctx->log, cmd->properties, name, action, val, ctx);
    cmd->silence_option_deprecations -= 1;
    if (r == M_PROPERTY_OK && is_property_set(action, val))
        mp_notify_property(ctx, (char *)name);
    return r;
}

int mp_property_do(const char *name, int action, void *val,
                   struct MPContext *ctx)
{
    int r = mp_property_do_silent(name, action, val, ctx);
    if (mp_msg_test(ctx->log, MSGL_V) && is_property_set(action, val)) {
        struct m_option ot = {0};
        void *data = val;
        switch (action) {
        case M_PROPERTY_SET_NODE:
            ot.type = &m_option_type_node;
            break;
        case M_PROPERTY_SET_STRING:
            ot.type = &m_option_type_string;
            data = &val;
            break;
        }
        char *t = ot.type ? m_option_print(&ot, data) : NULL;
        MP_VERBOSE(ctx, "Set property: %s%s%s -> %d\n",
                   name, t ? "=" : "", t ? t : "", r);
        talloc_free(t);
    }
    return r;
}

char *mp_property_expand_string(struct MPContext *mpctx, const char *str)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    return m_properties_expand_string(ctx->properties, str, mpctx);
}

// Before expanding properties, parse C-style escapes like "\n"
char *mp_property_expand_escaped_string(struct MPContext *mpctx, const char *str)
{
    void *tmp = talloc_new(NULL);
    bstr strb = bstr0(str);
    bstr dst = {0};
    while (strb.len) {
        if (!mp_append_escaped_string(tmp, &dst, &strb)) {
            talloc_free(tmp);
            return talloc_strdup(NULL, "(broken escape sequences)");
        }
        // pass " through literally
        if (!bstr_eatstart0(&strb, "\""))
            break;
        bstr_xappend(tmp, &dst, bstr0("\""));
    }
    char *r = mp_property_expand_string(mpctx, dst.start);
    talloc_free(tmp);
    return r;
}

void property_print_help(struct MPContext *mpctx)
{
    struct command_ctx *ctx = mpctx->command_ctx;
    m_properties_print_help_list(mpctx->log, ctx->properties);
}

static bool reinit_filters(MPContext *mpctx, enum stream_type mediatype)
{
    switch (mediatype) {
    case STREAM_AUDIO:
        return reinit_audio_filters(mpctx) >= 0;
    }
    return false;
}

static const char *const filter_opt[STREAM_TYPE_COUNT] = {
    [STREAM_VIDEO] = "vf",
    [STREAM_AUDIO] = "af",
};

static int set_filters(struct MPContext *mpctx, enum stream_type mediatype,
                       struct m_obj_settings *new_chain)
{
    bstr option = bstr0(filter_opt[mediatype]);
    struct m_config_option *co = m_config_get_co(mpctx->mconfig, option);
    if (!co)
        return -1;

    struct m_obj_settings **list = co->data;
    struct m_obj_settings *old_settings = *list;
    *list = NULL;
    m_option_copy(co->opt, list, &new_chain);

    bool success = reinit_filters(mpctx, mediatype);

    if (success) {
        m_option_free(co->opt, &old_settings);
        mp_notify_property(mpctx, filter_opt[mediatype]);
    } else {
        m_option_free(co->opt, list);
        *list = old_settings;
        reinit_filters(mpctx, mediatype);
    }

    return success ? 0 : -1;
}

static struct track *find_track_with_url(struct MPContext *mpctx, int type,
                                         const char *url)
{
    for (int n = 0; n < mpctx->num_tracks; n++) {
        struct track *track = mpctx->tracks[n];
        if (track && track->type == type && track->is_external &&
            strcmp(track->external_filename, url) == 0)
            return track;
    }
    return NULL;
}

// Whether this property should react to key events generated by auto-repeat.
static bool check_property_autorepeat(char *property,  struct MPContext *mpctx)
{
    struct m_option prop = {0};
    if (mp_property_do(property, M_PROPERTY_GET_TYPE, &prop, mpctx) <= 0)
        return true;

    // This is a heuristic at best.
    if (prop.type == &m_option_type_flag || prop.type == &m_option_type_choice)
        return false;

    return true;
}

// Whether changes to this property (add/cycle cmds) benefit from cmd->scale
static bool check_property_scalable(char *property, struct MPContext *mpctx)
{
    struct m_option prop = {0};
    if (mp_property_do(property, M_PROPERTY_GET_TYPE, &prop, mpctx) <= 0)
        return true;

    // These properties are backed by a floating-point number
    return prop.type == &m_option_type_float ||
           prop.type == &m_option_type_double ||
           prop.type == &m_option_type_time ||
           prop.type == &m_option_type_aspect;
}

static void show_property_status(struct mp_cmd_ctx *cmd, const char *name, int r)
{
    struct MPContext *mpctx = cmd->mpctx;
    struct MPOpts *opts = mpctx->opts;
    int osd_duration = opts->osd_duration;

    if (r == M_PROPERTY_OK || r == M_PROPERTY_UNAVAILABLE) {
        if (r == M_PROPERTY_UNAVAILABLE)
            cmd->success = false;
    } else if (r == M_PROPERTY_UNKNOWN) {
        set_osd_msg(mpctx, 1, osd_duration, "Unknown property: '%s'", name);
        cmd->success = false;
    } else if (r <= 0) {
        set_osd_msg(mpctx, 1, osd_duration, "Failed to set property '%s'",
                    name);
        cmd->success = false;
    }
}

static void change_property_cmd(struct mp_cmd_ctx *cmd,
                                const char *name, int action, void *arg)
{
    int r = mp_property_do(name, action, arg, cmd->mpctx);
    show_property_status(cmd, name, r);
}

static bool compare_values(struct m_option *type, void *a, void *b)
{
    // Since there is no m_option_equals() or anything similar, we convert all
    // values to a common, unambiguous representation - strings.
    char *as = m_option_print(type, a);
    char *bs = m_option_print(type, b);
    bool res = bstr_equals(bstr0(as), bstr0(bs)); // treat as "" on failure
    talloc_free(as);
    talloc_free(bs);
    return res;
}

static void cmd_cycle_values(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int first = 0, dir = 1;

    if (strcmp(cmd->args[first].v.s, "!reverse") == 0) {
        first += 1;
        dir = -1;
    }

    const char *name = cmd->args[first].v.s;
    first += 1;

    if (first >= cmd->num_args) {
        MP_ERR(mpctx, "cycle-values command does not have any value arguments.\n");
        cmd->success = false;
        return;
    }

    struct m_option prop = {0};
    int r = mp_property_do(name, M_PROPERTY_GET_TYPE, &prop, mpctx);
    if (r <= 0) {
        show_property_status(cmd, name, r);
        return;
    }

    union m_option_value curval = {0};
    r = mp_property_do(name, M_PROPERTY_GET, &curval, mpctx);
    if (r <= 0) {
        show_property_status(cmd, name, r);
        return;
    }

    // Find the current value. Note that we even though compare_values() uses
    // strings internally, we need to convert the cycle-values arguments to
    // native anyway to "normalize" the value for comparison.
    int current = -1;
    for (int n = first; n < cmd->num_args; n++) {
        union m_option_value val = {0};
        if (m_option_parse(mpctx->log, &prop, bstr0(name),
                           bstr0(cmd->args[n].v.s), &val) < 0)
            continue;

        if (compare_values(&prop, &curval, &val))
            current = n;

        m_option_free(&prop, &val);

        if (current >= 0)
            break;
    }

    m_option_free(&prop, &curval);

    if (current >= 0) {
        current += dir;
        if (current < first)
            current = cmd->num_args - 1;
        if (current >= cmd->num_args)
            current = first;
    } else {
        MP_VERBOSE(mpctx, "Current value not found. Picking default.\n");
        current = dir > 0 ? first : cmd->num_args - 1;
    }

    change_property_cmd(cmd, name, M_PROPERTY_SET_STRING, cmd->args[current].v.s);
}

struct cmd_list_ctx {
    struct MPContext *mpctx;

    // actual list command
    struct mp_cmd_ctx *parent;

    bool current_valid;
    pthread_t current;
    bool completed_recursive;

    // list of sub commands yet to run
    struct mp_cmd **sub;
    int num_sub;
};

static void continue_cmd_list(struct cmd_list_ctx *list);

static void on_cmd_list_sub_completion(struct mp_cmd_ctx *cmd)
{
    struct cmd_list_ctx *list = cmd->on_completion_priv;

    if (list->current_valid && pthread_equal(list->current, pthread_self())) {
        list->completed_recursive = true;
    } else {
        continue_cmd_list(list);
    }
}

static void continue_cmd_list(struct cmd_list_ctx *list)
{
    while (list->parent->args[0].v.p) {
        struct mp_cmd *sub = list->parent->args[0].v.p;
        list->parent->args[0].v.p = sub->queue_next;

        ta_xset_parent(sub, NULL);

        if (sub->flags & MP_ASYNC_CMD) {
            // We run it "detached" (fire & forget)
            run_command(list->mpctx, sub, NULL, NULL, NULL);
        } else {
            // Run the next command once this one completes.

            list->completed_recursive = false;
            list->current_valid = true;
            list->current = pthread_self();

            run_command(list->mpctx, sub, NULL, on_cmd_list_sub_completion, list);

            list->current_valid = false;

            // run_command() either recursively calls the completion function,
            // or lets the command continue run in the background. If it was
            // completed recursively, we can just continue our loop. Otherwise
            // the completion handler will invoke this loop again elsewhere.
            // We could unconditionally call continue_cmd_list() in the handler
            // instead, but then stack depth would grow with list length.
            if (!list->completed_recursive)
                return;
        }
    }

    mp_cmd_ctx_complete(list->parent);
    talloc_free(list);
}

static void cmd_list(void *p)
{
    struct mp_cmd_ctx *cmd = p;

    cmd->completed = false;

    struct cmd_list_ctx *list = talloc_zero(NULL, struct cmd_list_ctx);
    list->mpctx = cmd->mpctx;
    list->parent = p;

    continue_cmd_list(list);
}

const struct mp_cmd_def mp_cmd_list = { "list", cmd_list, .exec_async = true };

// Signal that the command is complete now. This also deallocates cmd.
// You must call this function in a state where the core is locked for the
// current thread (e.g. from the main thread, or from within mp_dispatch_lock()).
// Completion means the command is finished, even if it errored or never ran.
// Keep in mind that calling this can execute further user command that can
// change arbitrary state (due to cmd_list).
void mp_cmd_ctx_complete(struct mp_cmd_ctx *cmd)
{
    cmd->completed = true;
    if (!cmd->success)
        mpv_free_node_contents(&cmd->result);
    if (cmd->on_completion)
        cmd->on_completion(cmd);
    if (cmd->abort)
        mp_abort_remove(cmd->mpctx, cmd->abort);
    mpv_free_node_contents(&cmd->result);
    talloc_free(cmd);
}

static void run_command_on_worker_thread(void *p)
{
    struct mp_cmd_ctx *ctx = p;
    struct MPContext *mpctx = ctx->mpctx;

    mp_core_lock(mpctx);

    bool exec_async = ctx->cmd->def->exec_async;
    ctx->cmd->def->handler(ctx);
    if (!exec_async)
        mp_cmd_ctx_complete(ctx);

    mpctx->outstanding_async -= 1;
    if (!mpctx->outstanding_async && mp_is_shutting_down(mpctx))
        mp_wakeup_core(mpctx);

    mp_core_unlock(mpctx);
}

// Run the given command. Upon command completion, on_completion is called. This
// can happen within the function, or for async commands, some time after the
// function returns (the caller is supposed to be able to handle both cases). In
// both cases, the callback will be called while the core is locked (i.e. you
// can access the core freely).
// If abort is non-NULL, then the caller creates the abort object. It must have
// been allocated with talloc. run_command() will register/unregister/destroy
// it. Must not be set if cmd->def->can_abort==false.
// on_completion_priv is copied to mp_cmd_ctx.on_completion_priv and can be
// accessed from the completion callback.
// The completion callback is invoked exactly once. If it's NULL, it's ignored.
// Ownership of cmd goes to the caller.
void run_command(struct MPContext *mpctx, struct mp_cmd *cmd,
                 struct mp_abort_entry *abort,
                 void (*on_completion)(struct mp_cmd_ctx *cmd),
                 void *on_completion_priv)
{
    struct mp_cmd_ctx *ctx = talloc(NULL, struct mp_cmd_ctx);
    *ctx = (struct mp_cmd_ctx){
        .mpctx = mpctx,
        .cmd = talloc_steal(ctx, cmd),
        .args = cmd->args,
        .num_args = cmd->nargs,
        .priv = cmd->def->priv,
        .abort = talloc_steal(ctx, abort),
        .success = true,
        .completed = true,
        .on_completion = on_completion,
        .on_completion_priv = on_completion_priv,
    };

    if (!ctx->abort && cmd->def->can_abort)
        ctx->abort = talloc_zero(ctx, struct mp_abort_entry);

    assert(cmd->def->can_abort == !!ctx->abort);

    if (ctx->abort) {
        ctx->abort->coupled_to_playback |= cmd->def->abort_on_playback_end;
        mp_abort_add(mpctx, ctx->abort);
    }

    mp_cmd_dump(mpctx->log, cmd->def->is_ignore ? MSGL_TRACE : MSGL_DEBUG,
                "Run command:", cmd);

    if (cmd->flags & MP_EXPAND_PROPERTIES) {
        for (int n = 0; n < cmd->nargs; n++) {
            if (cmd->args[n].type->type == CONF_TYPE_STRING) {
                char *s = mp_property_expand_string(mpctx, cmd->args[n].v.s);
                if (!s) {
                    ctx->success = false;
                    mp_cmd_ctx_complete(ctx);
                    return;
                }
                talloc_free(cmd->args[n].v.s);
                cmd->args[n].v.s = s;
            }
        }
    }

    if (cmd->def->spawn_thread) {
        mpctx->outstanding_async += 1; // prevent that core disappears
        if (!mp_thread_pool_queue(mpctx->thread_pool,
                                  run_command_on_worker_thread, ctx))
        {
            mpctx->outstanding_async -= 1;
            ctx->success = false;
            mp_cmd_ctx_complete(ctx);
        }
    } else {
        bool exec_async = cmd->def->exec_async;
        cmd->def->handler(ctx);
        if (!exec_async)
            mp_cmd_ctx_complete(ctx);
    }
}

static void cmd_seek(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    double v = cmd->args[0].v.d * cmd->cmd->scale;
    int abs = cmd->args[1].v.i & 3;
    enum seek_precision precision = MPSEEK_DEFAULT;
    switch (((cmd->args[2].v.i | cmd->args[1].v.i) >> 3) & 3) {
    case 1: precision = MPSEEK_KEYFRAME; break;
    case 2: precision = MPSEEK_EXACT; break;
    }
    if (!mpctx->playback_initialized) {
        cmd->success = false;
        return;
    }

    mark_seek(mpctx);
    switch (abs) {
    case 0: { // Relative seek
        queue_seek(mpctx, MPSEEK_RELATIVE, v, precision, MPSEEK_FLAG_DELAY);
        set_osd_function(mpctx, (v > 0) ? OSD_FFW : OSD_REW);
        break;
    }
    case 1: { // Absolute seek by percentage
        double ratio = v / 100.0;
        double cur_pos = get_current_pos_ratio(mpctx, false);
        queue_seek(mpctx, MPSEEK_FACTOR, ratio, precision, MPSEEK_FLAG_DELAY);
        set_osd_function(mpctx, cur_pos < ratio ? OSD_FFW : OSD_REW);
        break;
    }
    case 2: { // Absolute seek to a timestamp in seconds
        if (v < 0) {
            // Seek from end
            double len = get_time_length(mpctx);
            if (len < 0) {
                cmd->success = false;
                return;
            }
            v = MPMAX(0, len + v);
        }
        queue_seek(mpctx, MPSEEK_ABSOLUTE, v, precision, MPSEEK_FLAG_DELAY);
        set_osd_function(mpctx,
                         v > get_current_time(mpctx) ? OSD_FFW : OSD_REW);
        break;
    }
    case 3: { // Relative seek by percentage
        queue_seek(mpctx, MPSEEK_FACTOR,
                   get_current_pos_ratio(mpctx, false) + v / 100.0,
                   precision, MPSEEK_FLAG_DELAY);
        set_osd_function(mpctx, v > 0 ? OSD_FFW : OSD_REW);
        break;
    }}

}

static void cmd_revert_seek(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    struct command_ctx *cmdctx = mpctx->command_ctx;

    if (!mpctx->playback_initialized) {
        cmd->success = false;
        return;
    }

    double oldpts = cmdctx->last_seek_pts;
    if (cmdctx->marked_pts != MP_NOPTS_VALUE)
        oldpts = cmdctx->marked_pts;
    if (cmd->args[0].v.i == 1) {
        cmdctx->marked_pts = get_current_time(mpctx);
    } else if (oldpts != MP_NOPTS_VALUE) {
        cmdctx->last_seek_pts = get_current_time(mpctx);
        cmdctx->marked_pts = MP_NOPTS_VALUE;
        queue_seek(mpctx, MPSEEK_ABSOLUTE, oldpts, MPSEEK_EXACT,
                   MPSEEK_FLAG_DELAY);
        set_osd_function(mpctx, OSD_REW);
    } else {
        cmd->success = false;
    }
}

static void cmd_set(void *p)
{
    struct mp_cmd_ctx *cmd = p;

    change_property_cmd(cmd, cmd->args[0].v.s,
                        M_PROPERTY_SET_STRING, cmd->args[1].v.s);
}

static void cmd_change_list(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    char *name = cmd->args[0].v.s;
    char *op = cmd->args[1].v.s;
    char *value = cmd->args[2].v.s;
    int osd_duration = mpctx->opts->osd_duration;

    struct m_config_option *co = m_config_get_co(mpctx->mconfig, bstr0(name));
    if (!co) {
        set_osd_msg(mpctx, 1, osd_duration, "Unknown option: '%s'", name);
        cmd->success = false;
        return;
    }

    const struct m_option_type *type = co->opt->type;
    bool found = false;
    for (int i = 0; type->actions && type->actions[i].name; i++) {
        const struct m_option_action *action = &type->actions[i];
        if (strcmp(action->name, op) == 0)
            found = true;
    }
    if (!found) {
        set_osd_msg(mpctx, 1, osd_duration, "Unknown action: '%s'", op);
        cmd->success = false;
        return;
    }

    char *optname = mp_tprintf(80, "%s-%s", name, op); // the dirty truth
    int r = m_config_set_option_cli(mpctx->mconfig, bstr0(optname),
                                    bstr0(value), M_SETOPT_RUNTIME);
    if (r < 0) {
        set_osd_msg(mpctx, 1, osd_duration,
                    "Failed setting option: '%s'", name);
        cmd->success = false;
        return;
    }

}

static void cmd_add_cycle(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    bool is_cycle = !!cmd->priv;

    char *property = cmd->args[0].v.s;
    if (cmd->cmd->repeated && !check_property_autorepeat(property, mpctx)) {
        MP_VERBOSE(mpctx, "Dropping command '%.*s' from auto-repeated key.\n",
                   BSTR_P(cmd->cmd->original));
        return;
    }

    double scale = 1;
    int scale_units = cmd->cmd->scale_units;
    if (check_property_scalable(property, mpctx)) {
        scale = cmd->cmd->scale;
        scale_units = 1;
    }

    for (int i = 0; i < scale_units; i++) {
        struct m_property_switch_arg s = {
            .inc = cmd->args[1].v.d * scale,
            .wrap = is_cycle,
        };
        change_property_cmd(cmd, property, M_PROPERTY_SWITCH, &s);
        if (!cmd->success)
            return;
    }
}

static void cmd_multiply(void *p)
{
    struct mp_cmd_ctx *cmd = p;

    change_property_cmd(cmd, cmd->args[0].v.s,
                        M_PROPERTY_MULTIPLY, &cmd->args[1].v.d);
}

static void cmd_frame_step(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    if (!mpctx->playback_initialized) {
        cmd->success = false;
        return;
    }

    if (cmd->cmd->is_up_down) {
        if (cmd->cmd->is_up) {
            if (mpctx->step_frames < 1)
                set_pause_state(mpctx, true);
        } else {
            if (cmd->cmd->repeated) {
                set_pause_state(mpctx, false);
            } else {
                add_step_frame(mpctx, 1);
            }
        }
    } else {
        add_step_frame(mpctx, 1);
    }
}

static void cmd_frame_back_step(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    if (!mpctx->playback_initialized) {
        cmd->success = false;
        return;
    }

    add_step_frame(mpctx, -1);
}

static void cmd_quit(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    bool write_watch_later = *(bool *)cmd->priv;
    if (write_watch_later || mpctx->opts->position_save_on_quit)
        mp_write_watch_later_conf(mpctx);
    mpctx->stop_play = PT_QUIT;
    mpctx->quit_custom_rc = cmd->args[0].v.i;
    mpctx->has_quit_custom_rc = true;
    mp_wakeup_core(mpctx);
}

static void cmd_playlist_next_prev(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int dir = *(int *)cmd->priv;
    int force = cmd->args[0].v.i;

    struct playlist_entry *e = mp_next_file(mpctx, dir, force, true);
    if (!e && !force) {
        cmd->success = false;
        return;
    }

    mp_set_playlist_entry(mpctx, e);

// HISONA ...
/*
    if (cmd->on_osd & MP_ON_OSD_MSG)
        mpctx->add_osd_seek_info |= OSD_SEEK_INFO_CURRENT_FILE;
*/

}

static void cmd_print_text(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    MP_INFO(mpctx, "%s\n", cmd->args[0].v.s);
}

static void cmd_show_text(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int osd_duration = mpctx->opts->osd_duration;

    // if no argument supplied use default osd_duration, else <arg> ms.
    set_osd_msg(mpctx, cmd->args[2].v.i,
                (cmd->args[1].v.i < 0 ? osd_duration : cmd->args[1].v.i),
                "%s", cmd->args[0].v.s);
}

static void cmd_expand_text(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    cmd->result = (mpv_node){
        .format = MPV_FORMAT_STRING,
        .u.string = mp_property_expand_string(mpctx, cmd->args[0].v.s)
    };
}

static void cmd_loadfile(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    char *filename = cmd->args[0].v.s;
    int append = cmd->args[1].v.i;

    if (!append)
        playlist_clear(mpctx->playlist);

    struct playlist_entry *entry = playlist_entry_new(filename);
    if (cmd->args[2].v.str_list) {
        char **pairs = cmd->args[2].v.str_list;
        for (int i = 0; pairs[i] && pairs[i + 1]; i += 2)
            playlist_entry_add_param(entry, bstr0(pairs[i]), bstr0(pairs[i + 1]));
    }
    playlist_add(mpctx->playlist, entry);

    if (!append || (append == 2 && !mpctx->playlist->current)) {
        if (mpctx->opts->position_save_on_quit) // requested in issue #1148
            mp_write_watch_later_conf(mpctx);
        mp_set_playlist_entry(mpctx, entry);
    }
    mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
    mp_wakeup_core(mpctx);
}

static void cmd_loadlist(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    char *filename = cmd->args[0].v.s;
    bool append = cmd->args[1].v.i;

    struct playlist *pl = playlist_parse_file(filename, cmd->abort->cancel,
                                              mpctx->global);
    if (pl) {
        prepare_playlist(mpctx, pl);
        struct playlist_entry *new = pl->current;
        if (!append)
            playlist_clear(mpctx->playlist);
        playlist_append_entries(mpctx->playlist, pl);
        talloc_free(pl);

        if (!append && mpctx->playlist->first)
            mp_set_playlist_entry(mpctx, new ? new : mpctx->playlist->first);

        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        mp_wakeup_core(mpctx);
    } else {
        MP_ERR(mpctx, "Unable to load playlist %s.\n", filename);
        cmd->success = false;
    }
}

static void cmd_playlist_clear(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    // Supposed to clear the playlist, except the currently played item.
    if (mpctx->playlist->current_was_replaced)
        mpctx->playlist->current = NULL;
    while (mpctx->playlist->first) {
        struct playlist_entry *e = mpctx->playlist->first;
        if (e == mpctx->playlist->current) {
            e = e->next;
            if (!e)
                break;
        }
        playlist_remove(mpctx->playlist, e);
    }
    mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
    mp_wakeup_core(mpctx);
}

static void cmd_playlist_remove(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    struct playlist_entry *e = playlist_entry_from_index(mpctx->playlist,
                                                         cmd->args[0].v.i);
    if (cmd->args[0].v.i < 0)
        e = mpctx->playlist->current;
    if (!e) {
        cmd->success = false;
        return;
    }

    // Can't play a removed entry
    if (mpctx->playlist->current == e && !mpctx->stop_play)
        mpctx->stop_play = PT_NEXT_ENTRY;
    playlist_remove(mpctx->playlist, e);
    mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
    mp_wakeup_core(mpctx);
}

static void cmd_playlist_move(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    struct playlist_entry *e1 = playlist_entry_from_index(mpctx->playlist,
                                                          cmd->args[0].v.i);
    struct playlist_entry *e2 = playlist_entry_from_index(mpctx->playlist,
                                                          cmd->args[1].v.i);
    if (!e1) {
        cmd->success = false;
        return;
    }

    playlist_move(mpctx->playlist, e1, e2);
    mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
}

static void cmd_playlist_shuffle(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    playlist_shuffle(mpctx->playlist);
    mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
}

static void cmd_stop(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    playlist_clear(mpctx->playlist);
    if (mpctx->stop_play != PT_QUIT)
        mpctx->stop_play = PT_STOP;
    mp_wakeup_core(mpctx);
}

static void cmd_show_progress(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    mp_wakeup_core(mpctx);
}

static void cmd_track_add(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int type = *(int *)cmd->priv;

    if (mpctx->stop_play) {
        cmd->success = false;
        return;
    }

    if (cmd->args[1].v.i == 2) {
        struct track *t = find_track_with_url(mpctx, type, cmd->args[0].v.s);
        if (t) {
            if (mpctx->playback_initialized) {
                mp_switch_track(mpctx, t->type, t, FLAG_MARK_SELECTION);
                print_track_list(mpctx, "Track switched:");
            } else {
                mpctx->opts->stream_id[0][t->type] = t->user_tid;
            }
            return;
        }
    }
    int first = mp_add_external_file(mpctx, cmd->args[0].v.s, type,
                                     cmd->abort->cancel);
    if (first < 0) {
        cmd->success = false;
        return;
    }

    for (int n = first; n < mpctx->num_tracks; n++) {
        struct track *t = mpctx->tracks[n];
        if (cmd->args[1].v.i == 1) {
            t->no_default = true;
        } else if (n == first) {
            if (mpctx->playback_initialized) {
                mp_switch_track(mpctx, t->type, t, FLAG_MARK_SELECTION);
            } else {
                mpctx->opts->stream_id[0][t->type] = t->user_tid;
            }
        }
        char *title = cmd->args[2].v.s;
        if (title && title[0])
            t->title = talloc_strdup(t, title);
        char *lang = cmd->args[3].v.s;
        if (lang && lang[0])
            t->lang = talloc_strdup(t, lang);
    }

    if (mpctx->playback_initialized)
        print_track_list(mpctx, "Track added:");
}

static void cmd_track_remove(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int type = *(int *)cmd->priv;

    struct track *t = mp_track_by_tid(mpctx, type, cmd->args[0].v.i);
    if (!t) {
        cmd->success = false;
        return;
    }

    mp_remove_track(mpctx, t);
    if (mpctx->playback_initialized)
        print_track_list(mpctx, "Track removed:");
}

static void cmd_track_reload(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int type = *(int *)cmd->priv;

    if (!mpctx->playback_initialized) {
        MP_ERR(mpctx, "Cannot reload while not initialized.\n");
        cmd->success = false;
        return;
    }

    struct track *t = mp_track_by_tid(mpctx, type, cmd->args[0].v.i);
    int nt_num = -1;

    if (t && t->is_external && t->external_filename) {
        char *filename = talloc_strdup(NULL, t->external_filename);
        mp_remove_track(mpctx, t);
        nt_num = mp_add_external_file(mpctx, filename, type, cmd->abort->cancel);
        talloc_free(filename);
    }

    if (nt_num < 0) {
        cmd->success = false;
        return;
    }

    struct track *nt = mpctx->tracks[nt_num];
    mp_switch_track(mpctx, nt->type, nt, 0);
    print_track_list(mpctx, "Reloaded:");
}

static void cmd_run(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    char **args = talloc_zero_array(NULL, char *, cmd->num_args + 1);
    for (int n = 0; n < cmd->num_args; n++)
        args[n] = cmd->args[n].v.s;
    mp_subprocess_detached(mpctx->log, args);
    talloc_free(args);
}

struct subprocess_cb_ctx {
    struct mp_log *log;
    void* talloc_ctx;
    int64_t max_size;
    bool capture[3];
    bstr output[3];
};

static void subprocess_output(struct subprocess_cb_ctx *ctx, int fd,
                              char *data, size_t size)
{
    if (ctx->capture[fd]) {
        if (ctx->output[fd].len < ctx->max_size)
            bstr_xappend(ctx->talloc_ctx, &ctx->output[fd], (bstr){data, size});
    } else {
        int msgl = fd == 2 ? MSGL_ERR : MSGL_INFO;
        mp_msg(ctx->log, msgl, "%.*s", (int)size, data);
    }
}

static void subprocess_stdout(void *p, char *data, size_t size)
{
    struct subprocess_cb_ctx *ctx = p;
    subprocess_output(ctx, 1, data, size);
}

static void subprocess_stderr(void *p, char *data, size_t size)
{
    struct subprocess_cb_ctx *ctx = p;
    subprocess_output(ctx, 2, data, size);
}

static void cmd_subprocess(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    char **args = cmd->args[0].v.str_list;
    bool playback_only = cmd->args[1].v.i;

    if (!args || !args[0]) {
        MP_ERR(mpctx, "program name missing\n");
        cmd->success = false;
        return;
    }

    void *tmp = talloc_new(NULL);
    struct subprocess_cb_ctx ctx = {
        .log = mp_log_new(tmp, mpctx->log, cmd->cmd->sender),
        .talloc_ctx = tmp,
        .max_size = cmd->args[2].v.i,
        .capture = {0, cmd->args[3].v.i, cmd->args[4].v.i},
    };

    pthread_mutex_lock(&mpctx->abort_lock);
    cmd->abort->coupled_to_playback = playback_only;
    mp_abort_recheck_locked(mpctx, cmd->abort);
    pthread_mutex_unlock(&mpctx->abort_lock);

    mp_core_unlock(mpctx);

    char *error = NULL;
    int status = mp_subprocess(args, cmd->abort->cancel, &ctx,
                               subprocess_stdout, subprocess_stderr, &error);

    mp_core_lock(mpctx);

    struct mpv_node *res = &cmd->result;
    node_init(res, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_int64(res, "status", status);
    node_map_add_flag(res, "killed_by_us", status == MP_SUBPROCESS_EKILLED_BY_US);
    node_map_add_string(res, "error_string", error ? error : "");
    const char *sname[] = {NULL, "stdout", "stderr"};
    for (int n = 1; n < 3; n++) {
        if (!ctx.capture[n])
            continue;
        struct mpv_byte_array *ba =
            node_map_add(res, sname[n], MPV_FORMAT_BYTE_ARRAY)->u.ba;
        *ba = (struct mpv_byte_array){
            .data = talloc_steal(ba, ctx.output[n].start),
            .size = ctx.output[n].len,
        };
    }

    talloc_free(tmp);
}

static void cmd_enable_input_section(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    mp_input_enable_section(mpctx->input, cmd->args[0].v.s, cmd->args[1].v.i);
}

static void cmd_disable_input_section(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    mp_input_disable_section(mpctx->input, cmd->args[0].v.s);
}

static void cmd_define_input_section(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    mp_input_define_section(mpctx->input, cmd->args[0].v.s, "<api>",
                            cmd->args[1].v.s, !cmd->args[2].v.i,
                            cmd->cmd->sender);
}

static void cmd_ab_loop(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int osd_duration = mpctx->opts->osd_duration;

    double now = get_current_time(mpctx);
    if (mpctx->opts->ab_loop[0] == MP_NOPTS_VALUE) {
        mp_property_do("ab-loop-a", M_PROPERTY_SET, &now, mpctx);
    } else if (mpctx->opts->ab_loop[1] == MP_NOPTS_VALUE) {
        mp_property_do("ab-loop-b", M_PROPERTY_SET, &now, mpctx);
    } else {
        now = MP_NOPTS_VALUE;
        mp_property_do("ab-loop-a", M_PROPERTY_SET, &now, mpctx);
        mp_property_do("ab-loop-b", M_PROPERTY_SET, &now, mpctx);
        set_osd_msg(mpctx, 1, osd_duration, "Clear A-B loop");
    }
}

static void cmd_drop_buffers(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    reset_audio_state(mpctx);

    if (mpctx->demuxer)
        demux_flush(mpctx->demuxer);
}

static void cmd_ao_reload(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    reload_audio_output(mpctx);
}

static void cmd_filter(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    cmd->success = false;
}

static void cmd_filter_command(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int type = *(int *)cmd->priv;

    struct mp_output_chain *chain = NULL;
    if (type == STREAM_AUDIO)
        chain = mpctx->ao_chain ? mpctx->ao_chain->filter : NULL;
    if (!chain) {
        cmd->success = false;
        return;
    }
    struct mp_filter_command filter_cmd = {
        .type = MP_FILTER_COMMAND_TEXT,
        .cmd = cmd->args[1].v.s,
        .arg = cmd->args[2].v.s,
    };

    cmd->success = mp_output_chain_command(chain, cmd->args[0].v.s, &filter_cmd);
}

static void cmd_script_binding(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct mp_cmd *incmd = cmd->cmd;
    struct MPContext *mpctx = cmd->mpctx;

    mpv_event_client_message event = {0};
    char *name = cmd->args[0].v.s;
    if (!name || !name[0]) {
        cmd->success = false;
        return;
    }

    char *sep = strchr(name, '/');
    char *target = NULL;
    char space[MAX_CLIENT_NAME];
    if (sep) {
        snprintf(space, sizeof(space), "%.*s", (int)(sep - name), name);
        target = space;
        name = sep + 1;
    }
    char state[3] = {'p', incmd->is_mouse_button ? 'm' : '-'};
    if (incmd->is_up_down)
        state[0] = incmd->repeated ? 'r' : (incmd->is_up ? 'u' : 'd');
    event.num_args = 4;
    event.args = (const char*[4]){"key-binding", name, state,
                                  incmd->key_name ? incmd->key_name : ""};
    if (mp_client_send_event_dup(mpctx, target,
                                 MPV_EVENT_CLIENT_MESSAGE, &event) < 0)
    {
        MP_VERBOSE(mpctx, "Can't find script '%s' when handling input.\n",
                    target ? target : "-");
        cmd->success = false;
    }
}

static void cmd_script_message_to(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    mpv_event_client_message *event = talloc_ptrtype(NULL, event);
    *event = (mpv_event_client_message){0};
    for (int n = 1; n < cmd->num_args; n++) {
        MP_TARRAY_APPEND(event, event->args, event->num_args,
                         talloc_strdup(event, cmd->args[n].v.s));
    }
    if (mp_client_send_event(mpctx, cmd->args[0].v.s, 0,
                                MPV_EVENT_CLIENT_MESSAGE, event) < 0)
    {
        MP_VERBOSE(mpctx, "Can't find script '%s' to send message to.\n",
                   cmd->args[0].v.s);
        cmd->success = false;
    }
}

static void cmd_script_message(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    const char **args = talloc_array(NULL, const char *, cmd->num_args);
    mpv_event_client_message event = {.args = args};
    for (int n = 0; n < cmd->num_args; n++)
        event.args[event.num_args++] = cmd->args[n].v.s;
    mp_client_broadcast_event(mpctx, MPV_EVENT_CLIENT_MESSAGE, &event);
    talloc_free(args);
}

static void cmd_ignore(void *p)
{
}

static void cmd_write_watch_later_config(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    mp_write_watch_later_conf(mpctx);
}

static void cmd_hook_add(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    if (!cmd->cmd->sender) {
        MP_ERR(mpctx, "Can be used from client API only.\n");
        cmd->success = false;
        return;
    }
    mp_hook_add(mpctx, cmd->cmd->sender, cmd->args[0].v.s, cmd->args[1].v.i,
                cmd->args[2].v.i, true);
}

static void cmd_hook_ack(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    if (!cmd->cmd->sender) {
        MP_ERR(mpctx, "Can be used from client API only.\n");
        cmd->success = false;
        return;
    }
    mp_hook_continue(mpctx, cmd->cmd->sender, cmd->args[0].v.i);
}

static void cmd_key(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;
    int action = *(int *)cmd->priv;

    const char *key_name = cmd->args[0].v.s;
    if (key_name[0] == '\0' && action == MP_KEY_STATE_UP) {
        mp_input_put_key_artificial(mpctx->input, MP_INPUT_RELEASE_ALL);
    } else {
        int code = mp_input_get_key_from_name(key_name);
        if (code < 0) {
            MP_ERR(mpctx, "%s is not a valid input name.\n", key_name);
            cmd->success = false;
            return;
        }
        mp_input_put_key_artificial(mpctx->input, code | action);
    }
}

static void cmd_apply_profile(void *p)
{
    struct mp_cmd_ctx *cmd = p;
    struct MPContext *mpctx = cmd->mpctx;

    char *profile = cmd->args[0].v.s;
    if (m_config_set_profile(mpctx->mconfig, profile, M_SETOPT_RUNTIME) < 0)
        cmd->success = false;
}

/* This array defines all known commands.
 * The first field the command name used in libmpv and input.conf.
 * The second field is the handler function (see mp_cmd_def.handler and
 * run_command()).
 * Then comes the definition of each argument. They are defined like options,
 * except that the result is parsed into mp_cmd.args[] (thus the option variable
 * is a field in the mp_cmd_arg union field). Arguments are optional if either
 * defval is set (usually via OPTDEF_ macros), or the MP_CMD_OPT_ARG flag is
 * set, or if it's the last argument and .vararg is set. If .vararg is set, the
 * command has an arbitrary number of arguments, all using the type indicated by
 * the last argument (they are appended to mp_cmd.args[] starting at the last
 * argument's index).
 * Arguments have named, which can be used by named argument functions, e.g. in
 * Lua with mp.command_native().
 */

// This does not specify the real destination of the command parameter values,
// it just provides a dummy for the OPT_ macros. The real destination is an
// array item  in mp_cmd.args[], using the index of the option definition.
#define OPT_BASE_STRUCT struct mp_cmd_arg

const struct mp_cmd_def mp_cmds[] = {
    { "ignore", cmd_ignore, .is_ignore = true },

    { "seek", cmd_seek,
        {
            OPT_TIME("target", v.d, 0),
            OPT_FLAGS("flags", v.i, 0,
                      ({"relative", 4|0}, {"-", 4|0},
                       {"absolute-percent", 4|1},
                       {"absolute", 4|2},
                       {"relative-percent", 4|3},
                       {"keyframes", 32|8},
                       {"exact", 32|16}),
                      OPTDEF_INT(4|0)),
            // backwards compatibility only
            OPT_CHOICE("legacy", v.i, MP_CMD_OPT_ARG,
                       ({"unused", 0}, {"default-precise", 0},
                        {"keyframes", 32|8},
                        {"exact", 32|16})),
        },
        .allow_auto_repeat = true,
        .scalable = true,
    },
    { "revert-seek", cmd_revert_seek,
        {OPT_FLAGS("flags", v.i, MP_CMD_OPT_ARG, ({"mark", 1}))},
    },
    { "quit", cmd_quit, { OPT_INT("code", v.i, MP_CMD_OPT_ARG) },
        .priv = &(const bool){0} },
    { "quit-watch-later", cmd_quit, { OPT_INT("code", v.i, MP_CMD_OPT_ARG) },
        .priv = &(const bool){1} },
    { "stop", cmd_stop, },
    { "frame-step", cmd_frame_step, .allow_auto_repeat = true,
        .on_updown = true },
    { "frame-back-step", cmd_frame_back_step, .allow_auto_repeat = true },
    { "playlist-next", cmd_playlist_next_prev,
        {
            OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG, ({"weak", 0},
                                                      {"force", 1})),
        },
        .priv = &(const int){1},
    },
    { "playlist-prev", cmd_playlist_next_prev,
        {
            OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG, ({"weak", 0},
                                                      {"force", 1})),
        },
        .priv = &(const int){-1},
    },
    { "playlist-shuffle", cmd_playlist_shuffle, },
    { "print-text", cmd_print_text, { OPT_STRING("text", v.s, 0) },
        .allow_auto_repeat = true },
    { "show-text", cmd_show_text, { OPT_STRING("text", v.s, 0),
                                    OPT_INT("duration", v.i, 0, OPTDEF_INT(-1)),
                                    OPT_INT("level", v.i, MP_CMD_OPT_ARG), },
        .allow_auto_repeat = true},
    { "expand-text", cmd_expand_text, { OPT_STRING("text", v.s, 0) } },
    { "show-progress", cmd_show_progress, .allow_auto_repeat = true},

    { "audio-add", cmd_track_add,
        {
            OPT_STRING("url", v.s, 0),
            OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG,
                       ({"select", 0}, {"auto", 1}, {"cached", 2})),
            OPT_STRING("title", v.s, MP_CMD_OPT_ARG),
            OPT_STRING("lang", v.s, MP_CMD_OPT_ARG),
        },
        .priv = &(const int){STREAM_AUDIO},
        .spawn_thread = true,
        .can_abort = true,
        .abort_on_playback_end = true,
    },

    { "audio-remove", cmd_track_remove, { OPT_INT("id", v.i, 0, OPTDEF_INT(-1)) },
        .priv = &(const int){STREAM_AUDIO}, },

    { "audio-reload", cmd_track_reload, { OPT_INT("id", v.i, 0, OPTDEF_INT(-1)) },
        .priv = &(const int){STREAM_AUDIO},
        .spawn_thread = true,
        .can_abort = true,
        .abort_on_playback_end = true,
    },

    { "loadfile", cmd_loadfile,
        {
            OPT_STRING("url", v.s, 0),
            OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG,
                       ({"replace", 0},
                        {"append", 1},
                        {"append-play", 2})),
            OPT_KEYVALUELIST("options", v.str_list, MP_CMD_OPT_ARG),
        },
    },
    { "loadlist", cmd_loadlist, { OPT_STRING("url", v.s, 0),
                                  OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG,
                                             ({"replace", 0}, {"append", 1})), },
        .spawn_thread = true,
        .can_abort = true,
    },
    { "playlist-clear", cmd_playlist_clear },
    { "playlist-remove", cmd_playlist_remove,
        {OPT_CHOICE_OR_INT("index", v.i, MP_CMD_OPT_ARG, 0, INT_MAX,
                           ({"current", -1}))},
    },

    { "playlist-move", cmd_playlist_move,  { OPT_INT("index1", v.i, 0),
                                             OPT_INT("index2", v.i, 0), }},

    { "run", cmd_run, { OPT_STRING("command", v.s, 0),
                        OPT_STRING("args", v.s, 0), },
        .vararg = true,
    },

    { "subprocess", cmd_subprocess,
        {
            OPT_STRINGLIST("args", v.str_list, 0),
            OPT_FLAG("playback_only", v.i, 0, OPTDEF_INT(1)),
            OPT_BYTE_SIZE("capture_size", v.i64, 0, 0, INT_MAX,
                          OPTDEF_INT64(64 * 1024 * 1024)),
            OPT_FLAG("capture_stdout", v.i, MP_CMD_OPT_ARG),
            OPT_FLAG("capture_stderr", v.i, MP_CMD_OPT_ARG),
        },
        .spawn_thread = true,
        .can_abort = true,
    },

    { "set", cmd_set, {OPT_STRING("name", v.s, 0), OPT_STRING("value", v.s, 0)}},
    { "change-list", cmd_change_list, { OPT_STRING("name", v.s, 0),
                                        OPT_STRING("operation", v.s, 0),
                                        OPT_STRING("value", v.s, 0) }},
    { "add", cmd_add_cycle, { OPT_STRING("name", v.s, 0),
                              OPT_DOUBLE("value", v.d, 0, OPTDEF_DOUBLE(1)), },
        .allow_auto_repeat = true,
        .scalable = true,
    },
    { "cycle", cmd_add_cycle, { OPT_STRING("name", v.s, 0),
                                OPT_CYCLEDIR("value", v.d, 0, OPTDEF_DOUBLE(1)), },
        .allow_auto_repeat = true,
        .scalable = true,
        .priv = "",
    },
    { "multiply", cmd_multiply, { OPT_STRING("name", v.s, 0),
                                  OPT_DOUBLE("value", v.d, 0)},
        .allow_auto_repeat = true},

    { "cycle-values", cmd_cycle_values, { OPT_STRING("arg0", v.s, 0),
                                          OPT_STRING("arg1", v.s, 0),
                                          OPT_STRING("argN", v.s, 0), },
        .vararg = true},

    { "enable-section", cmd_enable_input_section,
        {
            OPT_STRING("name", v.s, 0),
            OPT_FLAGS("flags", v.i, MP_CMD_OPT_ARG,
                      ({"default", 0},
                       {"exclusive", MP_INPUT_EXCLUSIVE},
                       {"allow-hide-cursor", MP_INPUT_ALLOW_HIDE_CURSOR},
                       {"allow-vo-dragging", MP_INPUT_ALLOW_VO_DRAGGING})),
        }
    },
    { "disable-section", cmd_disable_input_section,
        {OPT_STRING("name", v.s, 0) }},
    { "define-section", cmd_define_input_section,
        {
            OPT_STRING("name", v.s, 0),
            OPT_STRING("contents", v.s, 0),
            OPT_CHOICE("flags", v.i, MP_CMD_OPT_ARG,
                       ({"default", 0}, {"force", 1})),
        },
    },

    { "ab-loop", cmd_ab_loop },

    { "drop-buffers", cmd_drop_buffers, },

    { "af", cmd_filter, { OPT_STRING("operation", v.s, 0),
                          OPT_STRING("value", v.s, 0), },
        .priv = &(const int){STREAM_AUDIO} },

    { "af-command", cmd_filter_command, { OPT_STRING("label", v.s, 0),
                                          OPT_STRING("command", v.s, 0),
                                          OPT_STRING("argument", v.s, 0), },
        .priv = &(const int){STREAM_AUDIO} },

    { "ao-reload", cmd_ao_reload },

    { "script-binding", cmd_script_binding, { OPT_STRING("name", v.s, 0) },
        .allow_auto_repeat = true, .on_updown = true},

    { "script-message", cmd_script_message, { OPT_STRING("args", v.s, 0) },
        .vararg = true },
    { "script-message-to", cmd_script_message_to, { OPT_STRING("target", v.s, 0),
                                                    OPT_STRING("args", v.s, 0) },
        .vararg = true },

    { "write-watch-later-config", cmd_write_watch_later_config },

    { "hook-add", cmd_hook_add, { OPT_STRING("arg0", v.s, 0),
                                  OPT_INT("arg1", v.i, 0),
                                  OPT_INT("arg2", v.i, 0) }},
    { "hook-ack", cmd_hook_ack, { OPT_INT("arg0", v.i, 0) }},

    { "keypress", cmd_key, { OPT_STRING("name", v.s, 0) },
        .priv = &(const int){0}},
    { "keydown", cmd_key, { OPT_STRING("name", v.s, 0) },
        .priv = &(const int){MP_KEY_STATE_DOWN}},
    { "keyup", cmd_key, { OPT_STRING("name", v.s, MP_CMD_OPT_ARG) },
        .priv = &(const int){MP_KEY_STATE_UP}},

    { "apply-profile", cmd_apply_profile, {OPT_STRING("name", v.s, 0)} },

    {0}
};

#undef OPT_BASE_STRUCT
#undef ARG

void command_uninit(struct MPContext *mpctx)
{
    ao_hotplug_destroy(mpctx->command_ctx->hotplug);
    talloc_free(mpctx->command_ctx);
    mpctx->command_ctx = NULL;
}

void command_init(struct MPContext *mpctx)
{
    struct command_ctx *ctx = talloc(NULL, struct command_ctx);
    *ctx = (struct command_ctx){
        .last_seek_pts = MP_NOPTS_VALUE,
    };
    mpctx->command_ctx = ctx;

    int num_base = MP_ARRAY_SIZE(mp_properties_base);
    int num_opts = m_config_get_co_count(mpctx->mconfig);
    ctx->properties =
        talloc_zero_array(ctx, struct m_property, num_base + num_opts + 1);
    memcpy(ctx->properties, mp_properties_base, sizeof(mp_properties_base));

    int count = num_base;
    for (int n = 0; n < num_opts; n++) {
        struct m_config_option *co = m_config_get_co_index(mpctx->mconfig, n);
        assert(co->name[0]);
        if (co->opt->flags & M_OPT_NOPROP)
            continue;

        struct m_property prop = {
            .name = co->name,
            .call = mp_property_generic_option,
            .is_option = true,
        };

        if (co->opt->type == &m_option_type_alias) {
            prop.priv = co->opt->priv;

            prop.call = co->opt->deprecation_message ?
                            mp_property_deprecated_alias : mp_property_alias;
        }

        // The option might be covered by a manual property already.
        if (m_property_list_find(ctx->properties, prop.name))
            continue;

        ctx->properties[count++] = prop;
    }
}

static void command_event(struct MPContext *mpctx, int event, void *arg)
{
    struct command_ctx *ctx = mpctx->command_ctx;

    if (event == MPV_EVENT_START_FILE) {
        ctx->last_seek_pts = MP_NOPTS_VALUE;
        ctx->marked_pts = MP_NOPTS_VALUE;
    }

    if (event == MPV_EVENT_IDLE)
        ctx->is_idle = true;
    if (event == MPV_EVENT_START_FILE)
        ctx->is_idle = false;
    if (event == MPV_EVENT_END_FILE || event == MPV_EVENT_FILE_LOADED) {
        // Update chapters - does nothing if something else is visible.
    }
}

void handle_command_updates(struct MPContext *mpctx)
{
    struct command_ctx *ctx = mpctx->command_ctx;

    // This is a bit messy: ao_hotplug wakes up the player, and then we have
    // to recheck the state. Then the client(s) will read the property.
    if (ctx->hotplug && ao_hotplug_check_update(ctx->hotplug))
        mp_notify_property(mpctx, "audio-device-list");
}

void mp_notify(struct MPContext *mpctx, int event, void *arg)
{
    // The OSD can implicitly reference some properties.
    command_event(mpctx, event, arg);

    mp_client_broadcast_event(mpctx, event, arg);
}

static void update_priority(struct MPContext *mpctx)
{
#if HAVE_WIN32_DESKTOP
    struct MPOpts *opts = mpctx->opts;
    if (opts->w32_priority > 0)
        SetPriorityClass(GetCurrentProcess(), opts->w32_priority);
#endif
}

void mp_option_change_callback(void *ctx, struct m_config_option *co, int flags)
{
    struct MPContext *mpctx = ctx;
    struct command_ctx *cmd = mpctx->command_ctx;

    if (flags & UPDATE_TERM)
        mp_update_logging(mpctx, false);

    if (flags & UPDATE_INPUT) {
        mp_input_update_opts(mpctx->input);

        // Rather coarse change-detection, but sufficient effort.
        struct MPOpts *opts = mpctx->opts;
        if (!bstr_equals(bstr0(cmd->cur_ipc), bstr0(opts->ipc_path)) ||
            !bstr_equals(bstr0(cmd->cur_ipc_input), bstr0(opts->input_file)))
        {
            talloc_free(cmd->cur_ipc);
            talloc_free(cmd->cur_ipc_input);
            cmd->cur_ipc = talloc_strdup(cmd, opts->ipc_path);
            cmd->cur_ipc_input = talloc_strdup(cmd, opts->input_file);
            mp_uninit_ipc(mpctx->ipc_ctx);
            mpctx->ipc_ctx = mp_init_ipc(mpctx->clients, mpctx->global);
        }
    }

    if (flags & UPDATE_AUDIO)
        reload_audio_output(mpctx);

    if (flags & UPDATE_PRIORITY)
        update_priority(mpctx);

    if (flags & UPDATE_VOL)
        audio_update_volume(mpctx);

    if (flags & UPDATE_LAVFI_COMPLEX)
        update_lavfi_complex(mpctx);

}

void mp_notify_property(struct MPContext *mpctx, const char *property)
{
    mp_client_property_change(mpctx, property);
}
