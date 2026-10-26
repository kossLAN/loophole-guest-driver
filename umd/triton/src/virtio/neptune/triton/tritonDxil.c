/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXIL vertex-shader input-signature reader.
 *
 * The D3D12 runtime hands a DXIL-consuming driver the bare DXIL program
 * part, and the DDI's own input-element and signature records carry
 * registers only.  The semantic names a backend binds the input layout by
 * live in the program's LLVM 3.7 bitcode, as the dx.entryPoints metadata
 * every valid DXIL module carries:
 *
 *   !dx.entryPoints = !{!E, ...}
 *   !E  = !{fn, !"name", !S, ...}          !S = !{!In, !Out, !PatchConst}
 *   !In = !{!e0, !e1, ...}
 *   !eN = !{i32 id, !"Name", i8 compType, i8 semanticKind, !{i32 idx...},
 *           i8 interp, i32 rows, i8 cols, i32 startRow, i8 startCol, ...}
 *
 * Only what that needs is decoded: the bitstream, BLOCKINFO abbreviations,
 * the module's value numbering (globals, functions and aliases in record
 * order, then the module constants) and the module-level metadata block.
 * Function bodies are skipped by their block length.  Every length the
 * input claims is checked against the bits that remain before it is used.
 */

#include "tritonDxil.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC_DXIL 0x4C495844u

enum {
    BLOCK_BLOCKINFO = 0,
    BLOCK_MODULE    = 8,
    BLOCK_CONSTANTS = 11,
    BLOCK_METADATA  = 15,
};

/* The parent of the outermost blocks. */
#define BLOCK_TOP 0xFFFFFFFFu

enum {
    ABBREV_END_BLOCK      = 0,
    ABBREV_ENTER_SUBBLOCK = 1,
    ABBREV_DEFINE         = 2,
    ABBREV_UNABBREV       = 3,
};

enum { OP_LITERAL, OP_FIXED, OP_VBR, OP_ARRAY, OP_CHAR6, OP_BLOB };

enum {
    MODULE_CODE_GLOBALVAR = 7,
    MODULE_CODE_FUNCTION  = 8,
    MODULE_CODE_ALIAS     = 9,

    CST_CODE_SETTYPE      = 1,
    CST_CODE_NULL         = 2,
    CST_CODE_INTEGER      = 4,

    METADATA_STRING       = 1,
    METADATA_VALUE        = 2,
    METADATA_NODE         = 3,
    METADATA_NAME         = 4,
    METADATA_DISTINCT_NODE = 5,
    METADATA_LOCATION     = 7,
    METADATA_OLD_NODE     = 8,
    METADATA_OLD_FN_NODE  = 9,
    METADATA_NAMED_NODE   = 10,
    METADATA_GENERIC_DEBUG = 12,  /* 12..32: debug-info records */
    METADATA_LAST         = 32,
};

#define MAX_ABBREV_OPS  64
#define MAX_ABBREVS     4096
#define MAX_BLOCK_DEPTH 8
#define MAX_SIG_ROWS    32

typedef struct AbbrevOp { uint32_t kind; uint64_t val; } AbbrevOp;
typedef struct Abbrev   { uint32_t n; AbbrevOp op[MAX_ABBREV_OPS]; } Abbrev;

typedef struct Vec { void *p; size_t n, cap; } Vec;

static int vecReserve(Vec *v, size_t esz, size_t want)
{
    if (want <= v->cap)
        return 1;
    size_t cap = v->cap ? v->cap : 16;
    while (cap < want) {
        if (cap > ((size_t)-1) / 2 / esz)
            return 0;
        cap *= 2;
    }
    void *p = realloc(v->p, cap * esz);
    if (!p)
        return 0;
    v->p = p;
    v->cap = cap;
    return 1;
}

static int vecPush(Vec *v, size_t esz, const void *e)
{
    if (!vecReserve(v, esz, v->n + 1))
        return 0;
    memcpy((char *)v->p + v->n * esz, e, esz);
    v->n++;
    return 1;
}

typedef struct Value { uint8_t known; int64_t v; } Value;

enum { MD_OTHER, MD_STRING, MD_VALUE, MD_NODE };
typedef struct Md { uint8_t kind; size_t off, n; } Md;

typedef struct BlockInfo { uint32_t bid; Vec abbrevs; /* uint32_t */ } BlockInfo;

typedef struct Reader {
    const uint8_t *p;
    size_t nbits, pos;
    int err;

    Vec abbrevPool;   /* Abbrev */
    Vec blockInfo;    /* BlockInfo */
    Vec ops;          /* uint64_t: operands of the record just read */

    Vec values;       /* Value */
    Vec mds;          /* Md */
    Vec mdOps;        /* uint64_t */
    char mdName[32];
    int mdNameIsEntryPoints;
    Vec entryPoints;  /* uint64_t: !dx.entryPoints operands (metadata ids) */
    int haveEntryPoints;
} Reader;

/* ---------- bitstream ---------- */

static uint64_t readBits(Reader *r, unsigned n)
{
    if (r->err || n > 64 || r->pos > r->nbits || n > r->nbits - r->pos) {
        r->err = 1;
        return 0;
    }
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++, r->pos++)
        v |= (uint64_t)((r->p[r->pos >> 3] >> (r->pos & 7)) & 1) << i;
    return v;
}

static uint64_t readVbr(Reader *r, unsigned w)
{
    if (w < 2 || w > 32) {
        r->err = 1;
        return 0;
    }
    const uint64_t cont = 1ull << (w - 1);
    uint64_t v = 0;
    for (unsigned shift = 0; ; shift += w - 1) {
        const uint64_t chunk = readBits(r, w);
        const uint64_t data = chunk & (cont - 1);
        if (r->err || shift >= 64 || (shift && (data >> (64 - shift)))) {
            r->err = 1;
            return 0;
        }
        v |= data << shift;
        if (!(chunk & cont))
            return v;
    }
}

static void alignTo32(Reader *r)
{
    const size_t a = (r->pos + 31) & ~(size_t)31;
    if (a > r->nbits)
        r->err = 1;
    else
        r->pos = a;
}

static int64_t decodeSigned(uint64_t v)
{
    if (!(v & 1))
        return (int64_t)(v >> 1);
    if (v != 1)
        return -(int64_t)(v >> 1);
    return INT64_MIN;
}

static const char kChar6[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

/* ---------- abbreviations ---------- */

static Vec *blockInfoAbbrevs(Reader *r, uint32_t bid, int create)
{
    BlockInfo *bi = (BlockInfo *)r->blockInfo.p;
    for (size_t i = 0; i < r->blockInfo.n; i++)
        if (bi[i].bid == bid)
            return &bi[i].abbrevs;
    if (!create)
        return NULL;
    BlockInfo nb;
    memset(&nb, 0, sizeof(nb));
    nb.bid = bid;
    if (!vecPush(&r->blockInfo, sizeof(nb), &nb)) {
        r->err = 1;
        return NULL;
    }
    return &((BlockInfo *)r->blockInfo.p)[r->blockInfo.n - 1].abbrevs;
}

/* Read a DEFINE_ABBREV body into the pool and append its index to list. */
static void defineAbbrev(Reader *r, Vec *list)
{
    Abbrev a;
    memset(&a, 0, sizeof(a));
    const uint64_t n = readVbr(r, 5);
    if (r->err || n == 0 || n > MAX_ABBREV_OPS ||
        r->abbrevPool.n >= MAX_ABBREVS) {
        r->err = 1;
        return;
    }
    for (uint32_t i = 0; i < n && !r->err; i++) {
        AbbrevOp *op = &a.op[a.n++];
        if (readBits(r, 1)) {
            op->kind = OP_LITERAL;
            op->val = readVbr(r, 8);
            continue;
        }
        const uint64_t enc = readBits(r, 3);
        if (enc == 1 || enc == 2) {
            op->val = readVbr(r, 5);
            /* A zero-width field reads nothing: LLVM turns it into a
             * literal 0. */
            if (op->val == 0)
                op->kind = OP_LITERAL;
            else if ((enc == 1 && op->val > 64) || (enc == 2 && op->val > 32))
                r->err = 1;
            else
                op->kind = enc == 1 ? OP_FIXED : OP_VBR;
        } else if (enc == 3) {
            op->kind = OP_ARRAY;
        } else if (enc == 4) {
            op->kind = OP_CHAR6;
        } else if (enc == 5) {
            op->kind = OP_BLOB;
        } else {
            r->err = 1;
        }
    }
    if (r->err)
        return;
    /* An array is followed by exactly one element operand that consumes
     * input; a blob ends the abbreviation. */
    for (uint32_t i = 0; i < a.n; i++) {
        if (a.op[i].kind == OP_ARRAY) {
            if (i + 2 != a.n ||
                (a.op[i + 1].kind != OP_FIXED && a.op[i + 1].kind != OP_VBR &&
                 a.op[i + 1].kind != OP_CHAR6)) {
                r->err = 1;
                return;
            }
            break;
        }
        if (a.op[i].kind == OP_BLOB && i + 1 != a.n) {
            r->err = 1;
            return;
        }
    }
    const uint32_t index = (uint32_t)r->abbrevPool.n;
    if (!vecPush(&r->abbrevPool, sizeof(a), &a) ||
        !vecPush(list, sizeof(index), &index))
        r->err = 1;
}

static uint64_t readScalar(Reader *r, const AbbrevOp *op)
{
    switch (op->kind) {
    case OP_LITERAL: return op->val;
    case OP_FIXED:   return readBits(r, (unsigned)op->val);
    case OP_VBR:     return readVbr(r, (unsigned)op->val);
    case OP_CHAR6:   return (uint8_t)kChar6[readBits(r, 6)];
    default:         r->err = 1; return 0;
    }
}

static void pushOp(Reader *r, uint64_t v)
{
    if (!vecPush(&r->ops, sizeof(v), &v))
        r->err = 1;
}

/* Read one record into r->ops (code first). */
static void readRecord(Reader *r, uint64_t abbrevId, const Vec *local)
{
    r->ops.n = 0;
    if (abbrevId == ABBREV_UNABBREV) {
        const uint64_t code = readVbr(r, 6);
        const uint64_t n = readVbr(r, 6);
        /* Each operand takes at least 6 bits. */
        if (r->err || n > (r->nbits - r->pos) / 6) {
            r->err = 1;
            return;
        }
        pushOp(r, code);
        for (uint64_t i = 0; i < n && !r->err; i++)
            pushOp(r, readVbr(r, 6));
        return;
    }
    const uint64_t idx = abbrevId - 4;
    if (idx >= local->n) {
        r->err = 1;
        return;
    }
    const Abbrev *a =
        &((const Abbrev *)r->abbrevPool.p)[((const uint32_t *)local->p)[idx]];
    for (uint32_t i = 0; i < a->n && !r->err; i++) {
        const AbbrevOp *op = &a->op[i];
        if (op->kind == OP_ARRAY) {
            const AbbrevOp *elt = &a->op[i + 1];
            const unsigned minBits = elt->kind == OP_CHAR6 ? 6
                                   : elt->kind == OP_FIXED ? (unsigned)elt->val
                                   : (unsigned)elt->val;
            const uint64_t n = readVbr(r, 6);
            if (r->err || n > (r->nbits - r->pos) / minBits) {
                r->err = 1;
                return;
            }
            for (uint64_t k = 0; k < n && !r->err; k++)
                pushOp(r, readScalar(r, elt));
            return;
        }
        if (op->kind == OP_BLOB) {
            const uint64_t n = readVbr(r, 6);
            alignTo32(r);
            if (r->err || n > (r->nbits - r->pos) / 8) {
                r->err = 1;
                return;
            }
            for (uint64_t k = 0; k < n && !r->err; k++)
                pushOp(r, readBits(r, 8));
            alignTo32(r);
            return;
        }
        pushOp(r, readScalar(r, op));
    }
}

/* ---------- module contents ---------- */

static void pushValue(Reader *r, int known, int64_t v)
{
    Value val = { (uint8_t)known, v };
    if (r->values.n >= r->nbits) {
        r->err = 1;
        return;
    }
    if (!vecPush(&r->values, sizeof(val), &val))
        r->err = 1;
}

static void onModuleRecord(Reader *r, uint64_t code)
{
    if (code == MODULE_CODE_GLOBALVAR || code == MODULE_CODE_FUNCTION ||
        code == MODULE_CODE_ALIAS)
        pushValue(r, 0, 0);
}

static void onConstantsRecord(Reader *r, uint64_t code)
{
    const uint64_t *ops = (const uint64_t *)r->ops.p;
    if (code == CST_CODE_SETTYPE)
        return;
    if (code == CST_CODE_INTEGER && r->ops.n >= 2)
        pushValue(r, 1, decodeSigned(ops[1]));
    else if (code == CST_CODE_NULL)
        pushValue(r, 1, 0);
    else
        pushValue(r, 0, 0);
}

static void pushMd(Reader *r, uint8_t kind)
{
    Md md;
    md.kind = kind;
    md.off = r->mdOps.n;
    md.n = r->ops.n - 1;
    /* An abbreviation of literal operands yields operands without reading
     * input; real metadata never stores more operands than it has bits. */
    if (r->mdOps.n + md.n > r->nbits || r->mds.n >= r->nbits) {
        r->err = 1;
        return;
    }
    if (!vecReserve(&r->mdOps, sizeof(uint64_t), r->mdOps.n + md.n)) {
        r->err = 1;
        return;
    }
    if (md.n)
        memcpy((uint64_t *)r->mdOps.p + r->mdOps.n,
               (const uint64_t *)r->ops.p + 1, md.n * sizeof(uint64_t));
    r->mdOps.n += md.n;
    if (!vecPush(&r->mds, sizeof(md), &md))
        r->err = 1;
}

static void onMetadataRecord(Reader *r, uint64_t code)
{
    const uint64_t *ops = (const uint64_t *)r->ops.p;
    const size_t nops = r->ops.n - 1;
    switch (code) {
    case METADATA_STRING:
        pushMd(r, MD_STRING);
        break;
    case METADATA_VALUE:
        if (nops != 2)
            r->err = 1;
        else
            pushMd(r, MD_VALUE);
        break;
    case METADATA_NODE:
    case METADATA_DISTINCT_NODE:
        pushMd(r, MD_NODE);
        break;
    case METADATA_LOCATION:
    case METADATA_OLD_NODE:
    case METADATA_OLD_FN_NODE:
        pushMd(r, MD_OTHER);
        break;
    case METADATA_NAME: {
        static const char kEntryPoints[] = "dx.entryPoints";
        r->mdNameIsEntryPoints = nops == sizeof(kEntryPoints) - 1;
        for (size_t i = 0; i < nops && r->mdNameIsEntryPoints; i++)
            r->mdNameIsEntryPoints = ops[1 + i] == (uint8_t)kEntryPoints[i];
        break;
    }
    case METADATA_NAMED_NODE:
        if (r->mdNameIsEntryPoints && !r->haveEntryPoints) {
            if (!vecReserve(&r->entryPoints, sizeof(uint64_t), nops)) {
                r->err = 1;
                break;
            }
            if (nops)
                memcpy(r->entryPoints.p, ops + 1, nops * sizeof(uint64_t));
            r->entryPoints.n = nops;
            r->haveEntryPoints = 1;
        }
        r->mdNameIsEntryPoints = 0;
        break;
    default:
        /* Debug-info records each define a metadata id; KIND and the
         * attachment records do not.  Later LLVMs' records cannot appear in
         * DXIL, and guessing whether they define an id would misnumber
         * everything after them. */
        if (code >= METADATA_GENERIC_DEBUG && code <= METADATA_LAST)
            pushMd(r, MD_OTHER);
        else if (code > METADATA_LAST)
            r->err = 1;
        break;
    }
}

/* ---------- blocks ---------- */

static void parseBlock(Reader *r, uint32_t bid, unsigned width, size_t end,
                       int depth);

static void enterSubblock(Reader *r, uint32_t parent, size_t parentEnd,
                          int depth)
{
    const uint64_t bid = readVbr(r, 8);
    const uint64_t width = readVbr(r, 4);
    alignTo32(r);
    const uint64_t words = readBits(r, 32);
    if (r->err || width == 0 || width > 32 || r->pos > parentEnd ||
        words > (parentEnd - r->pos) / 32) {
        r->err = 1;
        return;
    }
    const size_t end = r->pos + (size_t)words * 32;
    const int wanted =
        bid == BLOCK_BLOCKINFO ||
        (parent == BLOCK_TOP && bid == BLOCK_MODULE) ||
        (parent == BLOCK_MODULE &&
         (bid == BLOCK_CONSTANTS || bid == BLOCK_METADATA));
    if (wanted && depth < MAX_BLOCK_DEPTH)
        parseBlock(r, (uint32_t)bid, (unsigned)width, end, depth + 1);
    if (!r->err && r->pos > end)
        r->err = 1;
    if (!r->err)
        r->pos = end;
}

static void parseBlock(Reader *r, uint32_t bid, unsigned width, size_t end,
                       int depth)
{
    Vec local = { 0 };
    const Vec *inherited = blockInfoAbbrevs(r, bid, 0);
    if (inherited && inherited->n) {
        if (!vecReserve(&local, sizeof(uint32_t), inherited->n))
            r->err = 1;
        else {
            memcpy(local.p, inherited->p, inherited->n * sizeof(uint32_t));
            local.n = inherited->n;
        }
    }
    int64_t curInfoBid = -1;
    while (!r->err) {
        if (r->pos >= end) {
            r->err = 1;
            break;
        }
        const uint64_t id = readBits(r, width);
        if (id == ABBREV_END_BLOCK) {
            alignTo32(r);
            break;
        }
        if (id == ABBREV_ENTER_SUBBLOCK) {
            enterSubblock(r, bid, end, depth);
            continue;
        }
        if (id == ABBREV_DEFINE) {
            if (bid == BLOCK_BLOCKINFO) {
                Vec *target = curInfoBid < 0
                    ? NULL : blockInfoAbbrevs(r, (uint32_t)curInfoBid, 1);
                if (!target)
                    r->err = 1;
                else
                    defineAbbrev(r, target);
            } else {
                defineAbbrev(r, &local);
            }
            continue;
        }
        readRecord(r, id, &local);
        if (r->err || !r->ops.n)
            break;
        const uint64_t code = ((const uint64_t *)r->ops.p)[0];
        switch (bid) {
        case BLOCK_BLOCKINFO:
            if (code == 1 /* SETBID */) {
                if (r->ops.n < 2 || ((const uint64_t *)r->ops.p)[1] > 0xFFFFFFFEu)
                    r->err = 1;
                else
                    curInfoBid = (int64_t)((const uint64_t *)r->ops.p)[1];
            }
            break;
        case BLOCK_MODULE:    onModuleRecord(r, code); break;
        case BLOCK_CONSTANTS: onConstantsRecord(r, code); break;
        case BLOCK_METADATA:  onMetadataRecord(r, code); break;
        default: break;
        }
    }
    free(local.p);
}

/* ---------- signature extraction ---------- */

/* Metadata id (0-based) -> entry, or NULL. */
static const Md *mdAt(const Reader *r, uint64_t id)
{
    return id < r->mds.n ? &((const Md *)r->mds.p)[id] : NULL;
}

/* A node operand: metadata id + 1, 0 for null. */
static const Md *mdOperand(const Reader *r, const Md *node, size_t i)
{
    if (!node || node->kind != MD_NODE || i >= node->n)
        return NULL;
    const uint64_t v = ((const uint64_t *)r->mdOps.p)[node->off + i];
    return v ? mdAt(r, v - 1) : NULL;
}

static int mdInt(const Reader *r, const Md *md, int64_t *out)
{
    if (!md || md->kind != MD_VALUE)
        return 0;
    const uint64_t vid = ((const uint64_t *)r->mdOps.p)[md->off + 1];
    if (vid >= r->values.n)
        return 0;
    const Value *v = &((const Value *)r->values.p)[vid];
    if (!v->known)
        return 0;
    *out = v->v;
    return 1;
}

static int mdIntOperand(const Reader *r, const Md *node, size_t i,
                        int64_t lo, int64_t hi, int64_t *out)
{
    return mdInt(r, mdOperand(r, node, i), out) && *out >= lo && *out <= hi;
}

typedef struct ElemInfo {
    const Md *name, *indices;
    int64_t kind, rows, startRow, startCol;
} ElemInfo;

static TritonDxilInputSig *buildSig(const Reader *r)
{
    if (!r->haveEntryPoints || !r->entryPoints.n)
        return NULL;
    const Md *entry = mdAt(r, ((const uint64_t *)r->entryPoints.p)[0]);
    if (!entry || entry->kind != MD_NODE || entry->n < 3)
        return NULL;
    /* Both a null signature list and a null input list mean no inputs. */
    const Md *sigs = mdOperand(r, entry, 2);
    const Md *inputs = sigs ? mdOperand(r, sigs, 0) : NULL;
    if ((sigs && sigs->kind != MD_NODE) || (inputs && inputs->kind != MD_NODE))
        return NULL;
    const size_t count = inputs ? inputs->n : 0;
    if (count > MAX_SIG_ROWS * 2)
        return NULL;

    ElemInfo info[MAX_SIG_ROWS * 2];
    size_t rows = 0, nameBytes = 0;
    for (size_t i = 0; i < count; i++) {
        const Md *e = mdOperand(r, inputs, i);
        ElemInfo *ei = &info[i];
        if (!e || e->kind != MD_NODE || e->n < 10)
            return NULL;
        ei->name = mdOperand(r, e, 1);
        ei->indices = mdOperand(r, e, 4);
        if (!ei->name || ei->name->kind != MD_STRING ||
            !ei->indices || ei->indices->kind != MD_NODE ||
            !mdIntOperand(r, e, 3, 0, 255, &ei->kind) ||
            !mdIntOperand(r, e, 6, 1, MAX_SIG_ROWS, &ei->rows) ||
            !mdIntOperand(r, e, 8, -1, MAX_SIG_ROWS - 1, &ei->startRow) ||
            !mdIntOperand(r, e, 9, -1, 3, &ei->startCol))
            return NULL;
        /* A system value without an index carries an empty index vector,
         * read as index 0 (DxilMDHelper::LoadSignatureElement). */
        if (ei->indices->n != (size_t)ei->rows &&
            !(ei->indices->n == 0 && ei->rows == 1))
            return NULL;
        for (size_t k = 0; k < ei->name->n; k++)
            if (((const uint64_t *)r->mdOps.p)[ei->name->off + k] == 0)
                return NULL;
        rows += (size_t)ei->rows;
        nameBytes += ei->name->n + 1;
    }

    const size_t cb = sizeof(TritonDxilInputSig) +
                      count * sizeof(TritonDxilInput) +
                      rows * sizeof(uint32_t) + nameBytes;
    unsigned char *mem = (unsigned char *)calloc(1, cb);
    if (!mem)
        return NULL;
    TritonDxilInputSig *sig = (TritonDxilInputSig *)mem;
    TritonDxilInput *elems = (TritonDxilInput *)(sig + 1);
    uint32_t *idx = (uint32_t *)(elems + count);
    char *names = (char *)(idx + rows);
    sig->Count = (uint32_t)count;
    sig->Elems = elems;
    for (size_t i = 0; i < count; i++) {
        const ElemInfo *ei = &info[i];
        TritonDxilInput *el = &elems[i];
        for (size_t k = 0; k < (size_t)ei->rows; k++) {
            int64_t v = 0;
            if (ei->indices->n &&
                !mdInt(r, mdOperand(r, ei->indices, k), &v)) {
                free(mem);
                return NULL;
            }
            if (v < 0 || v > 0xFFFFFFFFll) {
                free(mem);
                return NULL;
            }
            idx[k] = (uint32_t)v;
        }
        for (size_t k = 0; k < ei->name->n; k++)
            names[k] = (char)((const uint64_t *)r->mdOps.p)[ei->name->off + k];
        names[ei->name->n] = '\0';
        el->Name = names;
        el->SemanticIndex = idx;
        el->StartRow = (int32_t)ei->startRow;
        el->Rows = (uint32_t)ei->rows;
        el->StartCol = ei->startCol < 0 ? 0 : (uint32_t)ei->startCol;
        el->SemanticKind = (uint32_t)ei->kind;
        idx += ei->rows;
        names += ei->name->n + 1;
    }
    return sig;
}

/* ---------- entry points ---------- */

const void *tritonDxilFindProgram(const void *container, size_t cb,
                                  size_t *pcbProgram)
{
    const unsigned char *base = (const unsigned char *)container;
    if (!base || cb < 32)
        return NULL;
    uint32_t nParts;
    memcpy(&nParts, base + 28, 4);
    if (nParts > (cb - 32) / 4)
        return NULL;
    for (uint32_t i = 0; i < nParts; i++) {
        uint32_t off, fourcc, size;
        memcpy(&off, base + 32 + 4 * (size_t)i, 4);
        if (off > cb || cb - off < 8)
            continue;
        memcpy(&fourcc, base + off, 4);
        memcpy(&size, base + off + 4, 4);
        if (fourcc != FOURCC_DXIL || size > cb - off - 8)
            continue;
        *pcbProgram = size;
        return base + off + 8;
    }
    return NULL;
}

TritonDxilInputSig *tritonDxilReadInputSig(const void *program,
                                           size_t cbProgram)
{
    const unsigned char *p = (const unsigned char *)program;
    if (!p || cbProgram < 24)
        return NULL;
    uint32_t hdr[6];
    memcpy(hdr, p, sizeof(hdr));
    /* hdr: ProgramVersion, SizeInUint32, then the DxilBitcodeHeader:
     * 'DXIL', DxilVersion, BitcodeOffset (from that header), BitcodeSize. */
    if (hdr[2] != FOURCC_DXIL || hdr[4] < 16 ||
        (size_t)hdr[4] > cbProgram - 8 ||
        (size_t)hdr[5] > cbProgram - 8 - hdr[4] || hdr[5] < 8)
        return NULL;
    const unsigned char *bc = p + 8 + hdr[4];
    if (bc[0] != 'B' || bc[1] != 'C' || bc[2] != 0xC0 || bc[3] != 0xDE)
        return NULL;

    Reader r;
    memset(&r, 0, sizeof(r));
    r.p = bc;
    r.nbits = (size_t)hdr[5] * 8;
    r.pos = 32;
    while (!r.err && r.nbits - r.pos >= 32) {
        const uint64_t id = readBits(&r, 2);
        if (id != ABBREV_ENTER_SUBBLOCK)
            break;
        enterSubblock(&r, BLOCK_TOP, r.nbits, 0);
    }

    TritonDxilInputSig *sig = r.err ? NULL : buildSig(&r);

    BlockInfo *bi = (BlockInfo *)r.blockInfo.p;
    for (size_t i = 0; i < r.blockInfo.n; i++)
        free(bi[i].abbrevs.p);
    free(r.blockInfo.p);
    free(r.abbrevPool.p);
    free(r.ops.p);
    free(r.values.p);
    free(r.mds.p);
    free(r.mdOps.p);
    free(r.entryPoints.p);
    return sig;
}

void tritonDxilFreeInputSig(TritonDxilInputSig *sig)
{
    free(sig);
}

const char *tritonDxilInputSemantic(const TritonDxilInputSig *sig,
                                    uint32_t reg, uint32_t *pSemIndex)
{
    for (uint32_t i = 0; sig && i < sig->Count; i++) {
        const TritonDxilInput *e = &sig->Elems[i];
        if (e->StartRow < 0 || reg < (uint32_t)e->StartRow ||
            reg - (uint32_t)e->StartRow >= e->Rows)
            continue;
        *pSemIndex = e->SemanticIndex[reg - (uint32_t)e->StartRow];
        return e->Name;
    }
    return NULL;
}
