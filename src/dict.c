/**
 * @file dict.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief libyang dictionary for storing strings
 *
 * Copyright (c) 2015 - 2023 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include "dict.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "log.h"
#include "ly_common.h"

/* starting size of the dictionary */
#define LYDICT_MIN_SIZE 1024

/**
 * @brief Comparison callback for dictionary's hash table
 *
 * Implementation of ::lyht_value_equal_cb.
 */
static ly_bool
lydict_val_eq(void *val1_p, void *val2_p, ly_bool UNUSED(mod), void *cb_data)
{
    const char *str1, *str2;
    size_t *len1;

    str1 = ((struct ly_dict_rec *)val1_p)->value;
    str2 = ((struct ly_dict_rec *)val2_p)->value;
    len1 = cb_data;

    if (!strncmp(str1, str2, *len1) && !str2[*len1]) {
        return 1;
    }

    return 0;
}

void
lydict_init(struct ly_dict *dict)
{
    LY_CHECK_ARG_RET(NULL, dict, );

    dict->hash_tab = lyht_new(LYDICT_MIN_SIZE, sizeof(struct ly_dict_rec), lydict_val_eq, NULL, 1);
    LY_CHECK_ERR_RET(!dict->hash_tab, LOGINT(NULL), );
    pthread_mutex_init(&dict->lock, NULL);
}

void
lydict_clean(struct ly_dict *dict)
{
    struct ly_dict_rec *dict_rec = NULL;
    struct ly_ht_rec *rec = NULL;
    uint32_t hlist_idx;
    uint32_t rec_idx;

    if (!dict) {
        return;
    }

    LYHT_ITER_ALL_RECS(dict->hash_tab, hlist_idx, rec_idx, rec) {
        /*
         * this should not happen, all records inserted into
         * dictionary are supposed to be removed using lydict_remove()
         * before calling lydict_clean()
         */
        dict_rec = (struct ly_dict_rec *)rec->val;
        LOGWRN(NULL, "String \"%s\" not freed from the dictionary, refcount %" PRIu32 ".", dict_rec->value, dict_rec->refcount);
        /* if record wasn't removed before free string allocated for that record */
#ifdef NDEBUG
        free(dict_rec->value);
#endif
    }

    /* free table and destroy mutex */
    lyht_free(dict->hash_tab, NULL);
    pthread_mutex_destroy(&dict->lock);
}

static ly_bool
lydict_resize_val_eq(void *val1_p, void *val2_p, ly_bool mod, void *UNUSED(cb_data))
{
    const char *str1, *str2;

    LY_CHECK_ARG_RET(NULL, val1_p, val2_p, 0);

    str1 = ((struct ly_dict_rec *)val1_p)->value;
    str2 = ((struct ly_dict_rec *)val2_p)->value;

    if (mod) {
        /* used when inserting new values */
        if (strcmp(str1, str2) == 0) {
            return 1;
        }
    } else {
        /* used when finding the original value again in the resized table */
        if (str1 == str2) {
            return 1;
        }
    }

    return 0;
}

LIBYANG_API_DEF LY_ERR
lydict_remove(const struct ly_ctx *ctx, const char *value)
{
    LY_ERR ret = LY_SUCCESS;
    size_t len;
    uint32_t hash;
    struct ly_dict_rec rec, *match = NULL;
    char *val_p;
    struct ly_ctx_data *ctx_data;
    struct ly_dict *dict;

    if (!ctx || !value) {
        return LY_SUCCESS;
    }

    if (ctx->opts & LY_CTX_INT_IMMUTABLE) {
        ctx_data = ly_ctx_data_get(ctx);
        dict = ctx_data->data_dict;
    } else {
        dict = (struct ly_dict *)&ctx->dict;
    }

    LOGDBG(LY_LDGDICT, "removing \"%s\"", value);

    len = strlen(value);
    hash = lyht_hash(value, len);

    /* create record for lyht_find call */
    rec.value = (char *)value;
    rec.refcount = 0;

    pthread_mutex_lock(&dict->lock);
    /* set len as data for compare callback */
    lyht_set_cb_data(dict->hash_tab, (void *)&len);
    /* check if value is already inserted */
    ret = lyht_find(dict->hash_tab, &rec, hash, (void **)&match);

    if (ret == LY_SUCCESS) {
        LY_CHECK_ERR_GOTO(!match, LOGINT(ctx), cleanup);

        /* if value is already in dictionary, decrement reference counter */
        match->refcount--;
        if (match->refcount == 0) {
            /*
             * remove record
             * save pointer to stored string before lyht_remove to
             * free it after it is removed from hash table
             */
            val_p = match->value;
            ret = lyht_remove_with_resize_cb(dict->hash_tab, &rec, hash, lydict_resize_val_eq);
            free(val_p);
            LY_CHECK_ERR_GOTO(ret, LOGINT(ctx), cleanup);
        }
    } else if (ret == LY_ENOTFOUND) {
        LOGERR(ctx, LY_ENOTFOUND, "Value \"%s\" was not found in the dictionary.", value);
    } else {
        LOGINT(ctx);
    }

cleanup:
    pthread_mutex_unlock(&dict->lock);
    return ret;
}

static LY_ERR
dict_insert(struct ly_dict *dict, char *value, size_t len, ly_bool zerocopy, const char **str_p)
{
    LY_ERR ret = LY_SUCCESS;
    struct ly_dict_rec *match = NULL, rec;
    uint32_t hash;

    LOGDBG(LY_LDGDICT, "inserting \"%.*s\"", (int)len, value);

    hash = lyht_hash(value, len);
    /* set len as data for compare callback */
    lyht_set_cb_data(dict->hash_tab, (void *)&len);
    /* create record for lyht_insert */
    rec.value = value;
    rec.refcount = 1;

    ret = lyht_insert_with_resize_cb(dict->hash_tab, (void *)&rec, hash, lydict_resize_val_eq, (void **)&match);
    if (ret == LY_EEXIST) {
        match->refcount++;
        if (zerocopy) {
            free(value);
        }
        ret = LY_SUCCESS;
    } else if (ret == LY_SUCCESS) {
        if (!zerocopy) {
            /*
             * allocate string for new record
             * record is already inserted in hash table
             */
            match->value = malloc(sizeof *match->value * (len + 1));
            LY_CHECK_ERR_RET(!match->value, LOGMEM(NULL), LY_EMEM);
            if (len) {
                memcpy(match->value, value, len);
            }
            match->value[len] = '\0';
        }
    } else {
        /* lyht_insert returned error */
        if (zerocopy) {
            free(value);
        }
        return ret;
    }

    *str_p = match->value;

    return ret;
}

LIBYANG_API_DEF LY_ERR
lydict_insert(const struct ly_ctx *ctx, const char *value, size_t len, const char **str_p)
{
    LY_ERR rc;
    struct ly_ctx_data *ctx_data;
    struct ly_dict *dict;

    LY_CHECK_ARG_RET(ctx, ctx, str_p, LY_EINVAL);

    if (!value) {
        *str_p = NULL;
        return LY_SUCCESS;
    }

    if (!len) {
        len = strlen(value);
    }

    if (ctx->opts & LY_CTX_INT_IMMUTABLE) {
        ctx_data = ly_ctx_data_get(ctx);
        dict = ctx_data->data_dict;
    } else {
        dict = (struct ly_dict *)&ctx->dict;
    }

    pthread_mutex_lock(&dict->lock);
    rc = dict_insert(dict, (char *)value, len, 0, str_p);
    pthread_mutex_unlock(&dict->lock);

    return rc;
}

LIBYANG_API_DEF LY_ERR
lydict_insert_zc(const struct ly_ctx *ctx, char *value, const char **str_p)
{
    LY_ERR rc;
    struct ly_ctx_data *ctx_data;
    struct ly_dict *dict;

    LY_CHECK_ARG_RET(ctx, ctx, str_p, LY_EINVAL);

    if (!value) {
        *str_p = NULL;
        return LY_SUCCESS;
    }

    if (ctx->opts & LY_CTX_INT_IMMUTABLE) {
        ctx_data = ly_ctx_data_get(ctx);
        dict = ctx_data->data_dict;
    } else {
        dict = (struct ly_dict *)&ctx->dict;
    }

    pthread_mutex_lock(&dict->lock);
    rc = dict_insert(dict, value, strlen(value), 1, str_p);
    pthread_mutex_unlock(&dict->lock);

    return rc;
}

static LY_ERR
dict_dup(struct ly_dict *dict, char *value, const char **str_p)
{
    LY_ERR ret = LY_SUCCESS;
    struct ly_dict_rec *match = NULL, rec;
    uint32_t hash;

    /* set new callback to only compare memory addresses */
    lyht_value_equal_cb prev = lyht_set_cb(dict->hash_tab, lydict_resize_val_eq);

    LOGDBG(LY_LDGDICT, "duplicating %s", value);
    hash = lyht_hash(value, strlen(value));
    rec.value = value;

    ret = lyht_find(dict->hash_tab, (void *)&rec, hash, (void **)&match);
    if (ret == LY_SUCCESS) {
        /* record found, increase refcount */
        match->refcount++;
        *str_p = match->value;
    }

    /* restore callback */
    lyht_set_cb(dict->hash_tab, prev);

    return ret;
}

LIBYANG_API_DEF LY_ERR
lydict_dup(const struct ly_ctx *ctx, const char *value, const char **str_p)
{
    LY_ERR rc;
    struct ly_ctx_data *ctx_data;
    struct ly_dict *dict;

    LY_CHECK_ARG_RET(ctx, ctx, str_p, LY_EINVAL);

    if (!value) {
        *str_p = NULL;
        return LY_SUCCESS;
    }

    if (ctx->opts & LY_CTX_INT_IMMUTABLE) {
        ctx_data = ly_ctx_data_get(ctx);
        dict = ctx_data->data_dict;
    } else {
        dict = (struct ly_dict *)&ctx->dict;
    }

    pthread_mutex_lock(&dict->lock);
    rc = dict_dup(dict, (char *)value, str_p);
    pthread_mutex_unlock(&dict->lock);

    return rc;
}
