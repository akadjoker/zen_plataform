/*
 * ui_state.c - stable widget ids (FNV-1a + id stack) and the generic id-keyed
 *              state store (open-addressed hash).
 *
 * Widget identity must survive reordering between frames; the per-frame counter
 * (next_id) breaks as soon as a new widget is inserted above.  Hashed labels
 * scoped by the parent id stack fix this, exactly as ImGui does it.
 */
#include "ui_internal.h"
#include <string.h>
#include <stdlib.h>

/* ====================================================================== */
/*  FNV-1a hash (public domain)                                            */
/* ====================================================================== */

#define FNV_OFFSET 0x811c9dc5u
#define FNV_PRIME  0x01000193u

static uint32_t fnv_1a(const char *s, int len, uint32_t seed)
{
    uint32_t h = seed;
    for (int i = 0; i < len; ++i)
    {
        h ^= (uint8_t)s[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ====================================================================== */
/*  ID stack                                                               */
/* ====================================================================== */

void zui_push_id(UiContext *ui, const char *label)
{
    if (ui->id_sp < UI_ID_STACK_MAX)
    {
        ZuiId parent = ui->id_sp > 0 ? ui->id_stack[ui->id_sp - 1] : FNV_OFFSET;
        ui->id_stack[ui->id_sp] = fnv_1a(label, (int)strlen(label), parent);
        ui->id_sp++;
    }
}

void zui_push_id_i(UiContext *ui, int i)
{
    if (ui->id_sp < UI_ID_STACK_MAX)
    {
        ZuiId parent = ui->id_sp > 0 ? ui->id_stack[ui->id_sp - 1] : FNV_OFFSET;
        /* Hash the integer as 4 little-endian bytes */
        uint32_t h = parent;
        for (int b = 0; b < 4; ++b)
        {
            h ^= (uint8_t)(i >> (b * 8));
            h *= FNV_PRIME;
        }
        ui->id_stack[ui->id_sp] = h;
        ui->id_sp++;
    }
}

void zui_pop_id(UiContext *ui)
{
    if (ui->id_sp > 0)
        ui->id_sp--;
}

/* ====================================================================== */
/*  Label -> id (strip "##" / "###" conventions)                           */
/* ====================================================================== */

ZuiId zui_id(UiContext *ui, const char *label)
{
    /* "###fixed": text may vary, id is everything after "###" */
    const char *hash_part = NULL;
    int hash_len = 0;
    const char *display_part = label;
    int display_len = 0;

    if (label[0] == '#' && label[1] == '#' && label[2] == '#')
    {
        /* "###id": no visible text, only id */
        hash_part = label + 3;
        hash_len = (int)strlen(hash_part);
        display_part = NULL;
        display_len = 0;
    }
    else
    {
        /* Look for "##" in the label */
        const char *sep = strstr(label, "##");
        if (sep && sep[2] != '#')
        {
            display_part = label;
            display_len = (int)(sep - label);
            hash_part = sep + 2;
            hash_len = (int)strlen(hash_part);
        }
        else
        {
            /* No separator: the whole label is both display and id */
            display_part = label;
            display_len = (int)strlen(label);
        }
    }

    /* The id is computed from the hash_part only, scoped by the parent stack.
       If no hash_part was extracted, hash the whole label. */
    ZuiId parent = ui->id_sp > 0 ? ui->id_stack[ui->id_sp - 1] : FNV_OFFSET;
    const char *src = hash_part ? hash_part : label;
    int len = hash_part ? hash_len : (int)strlen(label);
    ZuiId id = fnv_1a(src, len, parent);

    (void)display_part;
    (void)display_len;

    return id;
}

/* ====================================================================== */
/*  State store (open-addressed hash, never evicts)                       */
/* ====================================================================== */

/* A mixing step so the index slot is not just id % size */
static uint32_t store_hash(ZuiId id)
{
    uint32_t h = (uint32_t)id;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static UiStoreEntry *store_at(UiContext *ui, int n)
{
    return &ui->store_blocks[n / UI_STORE_BLOCK][n % UI_STORE_BLOCK];
}

/* The index is kept at most half full and doubled when it would pass that; the entries stay where they are. */
static bool store_grow_index(UiContext *ui)
{
    int cap = ui->store_index_cap ? ui->store_index_cap * 2 : 256;
    UiStoreSlot *index = (UiStoreSlot *)calloc((size_t)cap, sizeof *index);
    if (!index)
        return false;
    const uint32_t mask = (uint32_t)cap - 1;
    for (int n = 0; n < ui->store_count; ++n)
    {
        UiStoreEntry *e = store_at(ui, n);
        uint32_t h = store_hash(e->id) & mask;
        while (index[h].entry)
            h = (h + 1) & mask;
        index[h].id = e->id;
        index[h].entry = e;
    }
    free(ui->store_index);
    ui->store_index = index;
    ui->store_index_cap = cap;
    return true;
}

/* The entry of `id`, made (empty: tag 0) when there is none. When the store is full (UI_STORE_MAX ids) or memory is
   gone it is a spill entry that starts empty each time, so the ids past the limit lose their state but never share
   another widget's: the store used to be 64 entries, and from the 65th id on every new one was handed slot 0. */
/* The id is not in the store: make its entry (not on the usual path, so it is kept out of the way of the lookup). */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static UiStoreEntry *store_insert(UiContext *ui, ZuiId id)
{
    if (ui->store_count >= UI_STORE_MAX)
        goto spill;
    if ((ui->store_count + 1) * 2 > ui->store_index_cap && !store_grow_index(ui))
        goto spill;
    if (ui->store_count == ui->store_blocks_n * UI_STORE_BLOCK)
    {
        UiStoreEntry **blocks = (UiStoreEntry **)realloc(ui->store_blocks, (size_t)(ui->store_blocks_n + 1) * sizeof *blocks);
        if (!blocks)
            goto spill;
        ui->store_blocks = blocks;
        UiStoreEntry *block = (UiStoreEntry *)calloc(UI_STORE_BLOCK, sizeof *block);
        if (!block)
            goto spill;
        ui->store_blocks[ui->store_blocks_n++] = block;
    }
    UiStoreEntry *e = store_at(ui, ui->store_count);
    e->id = id;
    const uint32_t mask = (uint32_t)ui->store_index_cap - 1;
    uint32_t h = store_hash(id) & mask;
    while (ui->store_index[h].entry)
        h = (h + 1) & mask;
    ui->store_index[h].id = id;
    ui->store_index[h].entry = e;
    ui->store_count++;
    return e;
spill:
    memset(&ui->store_spill, 0, sizeof ui->store_spill);
    ui->store_spill.id = id;
    return &ui->store_spill;
}

static UiStoreEntry *store_find_or_insert(UiContext *ui, ZuiId id)
{
    /* the usual call: the id is there (a widget asks every frame) */
    if (ui->store_index_cap)
    {
        const uint32_t mask = (uint32_t)ui->store_index_cap - 1;
        for (uint32_t h = store_hash(id) & mask; ui->store_index[h].entry != NULL; h = (h + 1) & mask)
            if (ui->store_index[h].id == id)
                return ui->store_index[h].entry;
    }
    return store_insert(ui, id);
}

void ui_state_free(UiContext *ui)
{
    for (int i = 0; i < ui->store_blocks_n; ++i)
        free(ui->store_blocks[i]);
    free(ui->store_blocks);
    free(ui->store_index);
    ui->store_blocks = NULL;
    ui->store_index = NULL;
    ui->store_blocks_n = ui->store_count = ui->store_index_cap = 0;
}

int *zui_state_int(UiContext *ui, ZuiId id, int defv)
{
    UiStoreEntry *e = store_find_or_insert(ui, id);
    if (e->tag == 0)
    {
        e->tag = 1;
        e->vi = defv;
    }
    return &e->vi;
}

float *zui_state_float(UiContext *ui, ZuiId id, float defv)
{
    UiStoreEntry *e = store_find_or_insert(ui, id);
    if (e->tag == 0)
    {
        e->tag = 2;
        e->vf = defv;
    }
    return &e->vf;
}

void *zui_state_ptr(UiContext *ui, ZuiId id)
{
    UiStoreEntry *e = store_find_or_insert(ui, id);
    if (e->tag == 0)
    {
        e->tag = 3;
        e->vp = NULL;
    }
    return &e->vp;
}

void zui_state_set_ptr(UiContext *ui, ZuiId id, void *p)
{
    UiStoreEntry *e = store_find_or_insert(ui, id);
    e->tag = 3;
    e->vp = p;
}
