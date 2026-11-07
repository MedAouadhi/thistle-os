/*
 * SPDX-License-Identifier: BSD-3-Clause
 * ThistleOS — the conversations a chat app keeps (chatkit_store.h)
 */
#include "chatkit/chatkit_store.h"

#include <stdlib.h>
#include <string.h>

void chatkit_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void touch(chatkit_store_t *st)
{
    st->generation++;
}

chatkit_conv_t *chatkit_find(chatkit_store_t *st, const char *conv)
{
    if (!conv) conv = "";
    for (int i = 0; i < st->n; i++) {
        if (strcmp(st->convs[i]->conv, conv) == 0) return st->convs[i];
    }
    return NULL;
}

static bool on_screen(const chatkit_store_t *st, const chatkit_conv_t *c)
{
    return st->viewing_set && strcmp(st->viewing, c->conv) == 0;
}

/* The conversation to forget for room: the longest quiet, one with nothing
 * unread before one with; never a sticky one or the one on screen. -1 for
 * none. */
static int forgettable(const chatkit_store_t *st)
{
    int best = -1;
    for (int i = 0; i < st->n; i++) {
        const chatkit_conv_t *c = st->convs[i];
        if (c->sticky || on_screen(st, c)) continue;
        if (best < 0) {
            best = i;
            continue;
        }
        const chatkit_conv_t *b = st->convs[best];
        bool c_unread = c->unread > 0, b_unread = b->unread > 0;
        if (c_unread != b_unread ? !c_unread : c->active < b->active) best = i;
    }
    return best;
}

chatkit_conv_t *chatkit_ensure(chatkit_store_t *st, const char *conv, const char *title, bool sticky)
{
    if (!conv) conv = "";
    chatkit_conv_t *c = chatkit_find(st, conv);
    if (!c) {
        if (st->n >= CHATKIT_CONVS) {
            int gone = forgettable(st);
            if (gone < 0) return NULL;
            if (st->release) st->release(st->convs[gone]);
            else free(st->convs[gone]);
            memmove(&st->convs[gone], &st->convs[gone + 1], sizeof st->convs[0] * (size_t)(st->n - gone - 1));
            st->n--;
        }
        c = st->alloc ? st->alloc(sizeof *c) : calloc(1, sizeof *c);
        if (!c) return NULL;
        memset(c, 0, sizeof *c);
        chatkit_copy(c->conv, sizeof c->conv, conv);
        c->active = ++st->seq;
        st->convs[st->n++] = c;
        touch(st);
    }
    if (sticky) c->sticky = true;
    if (title && title[0] && strcmp(c->title, title) != 0) {
        chatkit_copy(c->title, sizeof c->title, title);
        touch(st);
    }
    return c;
}

chatkit_msg_t *chatkit_add(chatkit_store_t *st, chatkit_conv_t *c, bool mine, const char *who,
                           const char *text, uint64_t when, uint32_t ref, chatkit_status_t status)
{
    if (!c) return NULL;
    chatkit_msg_t *m = &c->msgs[c->count % CHATKIT_HISTORY];
    memset(m, 0, sizeof *m);
    chatkit_copy(m->text, sizeof m->text, text);
    chatkit_copy(m->who, sizeof m->who, mine ? "" : who);
    m->when = when;
    m->ref = ref;
    m->status = (uint8_t)status;
    m->mine = mine;
    c->count++;
    c->active = ++st->seq;
    /* No more can be unread than the history keeps. */
    if (!mine && !on_screen(st, c) && c->unread < CHATKIT_HISTORY) c->unread++;
    touch(st);
    return m;
}

bool chatkit_set_status(chatkit_store_t *st, uint32_t ref, chatkit_status_t status)
{
    if (ref == 0) return false;
    for (int i = 0; i < st->n; i++) {
        chatkit_conv_t *c = st->convs[i];
        int shown = chatkit_shown(c);
        for (int j = 0; j < shown; j++) {
            chatkit_msg_t *m = chatkit_msg_mut(c, j);
            if (!m->mine || m->ref != ref) continue;
            if (m->status != status) {
                m->status = (uint8_t)status;
                touch(st);
            }
            return true;
        }
    }
    return false;
}

void chatkit_set_viewing(chatkit_store_t *st, const char *conv)
{
    st->viewing_set = conv != NULL;
    chatkit_copy(st->viewing, sizeof st->viewing, conv ? conv : "");
    if (!conv) return;
    chatkit_conv_t *c = chatkit_find(st, conv);
    if (c) chatkit_mark_read(st, c);
}

void chatkit_mark_read(chatkit_store_t *st, chatkit_conv_t *c)
{
    if (!c || c->unread == 0) return;
    c->unread = 0;
    touch(st);
}

uint32_t chatkit_unread(const chatkit_store_t *st)
{
    uint32_t n = 0;
    for (int i = 0; i < st->n; i++) n += st->convs[i]->unread;
    return n;
}

void chatkit_retitle(chatkit_store_t *st, chatkit_conv_t *c, const char *title)
{
    if (!c || !title || !title[0] || strcmp(c->title, title) == 0) return;
    chatkit_copy(c->title, sizeof c->title, title);
    int shown = chatkit_shown(c);
    for (int j = 0; j < shown; j++) {
        chatkit_msg_t *m = chatkit_msg_mut(c, j);
        if (!m->mine) chatkit_copy(m->who, sizeof m->who, title);
    }
    touch(st);
}

int chatkit_shown(const chatkit_conv_t *c)
{
    return c->count < CHATKIT_HISTORY ? (int)c->count : CHATKIT_HISTORY;
}

chatkit_msg_t *chatkit_msg_mut(chatkit_conv_t *c, int i)
{
    int shown = chatkit_shown(c);
    if (i < 0 || i >= shown) return NULL;
    uint32_t first = c->count - (uint32_t)shown;
    return &c->msgs[(first + (uint32_t)i) % CHATKIT_HISTORY];
}

const chatkit_msg_t *chatkit_msg(const chatkit_conv_t *c, int i)
{
    return chatkit_msg_mut((chatkit_conv_t *)c, i);
}

int chatkit_order(const chatkit_store_t *st, chatkit_conv_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < st->n && n < max; i++) out[n++] = st->convs[i];
    /* Few enough for an insertion sort. */
    for (int i = 1; i < n; i++) {
        chatkit_conv_t *c = out[i];
        int j = i - 1;
        while (j >= 0 && out[j]->active < c->active) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = c;
    }
    return n;
}

void chatkit_clear(chatkit_store_t *st)
{
    for (int i = 0; i < st->n; i++) {
        if (st->release) st->release(st->convs[i]);
        else free(st->convs[i]);
    }
    void *(*alloc)(size_t) = st->alloc;
    void (*release)(void *) = st->release;
    memset(st, 0, sizeof *st);
    st->alloc = alloc;
    st->release = release;
}
