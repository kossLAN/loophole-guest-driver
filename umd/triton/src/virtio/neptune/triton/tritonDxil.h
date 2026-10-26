/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXIL vertex-shader input-signature reader (tritonDxil.c).
 */

#ifndef TRITON_DXIL_H_INCLUDED
#define TRITON_DXIL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TritonDxilInput {
    const char     *Name;           /* SemanticName, NUL-terminated */
    const uint32_t *SemanticIndex;  /* one per row */
    int32_t         StartRow;       /* -1: not register-allocated (SV_VertexID) */
    uint32_t        Rows;
    uint32_t        StartCol;
    uint32_t        SemanticKind;   /* DXIL SemanticKind; 0 = arbitrary */
} TritonDxilInput;

typedef struct TritonDxilInputSig {
    uint32_t         Count;
    TritonDxilInput *Elems;
} TritonDxilInputSig;

/* The DXIL program part of a DXBC-magic container, or NULL. */
const void *tritonDxilFindProgram(const void *container, size_t cbContainer,
                                  size_t *pcbProgram);

/* Read the input signature of a DXIL program part (DxilProgramHeader onward)
 * from its dx.entryPoints metadata.  NULL when the part is malformed or
 * carries no entry point; a shader without inputs yields Count == 0. */
TritonDxilInputSig *tritonDxilReadInputSig(const void *program,
                                           size_t cbProgram);

void tritonDxilFreeInputSig(TritonDxilInputSig *sig);

/* Semantic name and index the shader reads at input register reg, or NULL
 * when no element occupies that register. */
const char *tritonDxilInputSemantic(const TritonDxilInputSig *sig,
                                    uint32_t reg, uint32_t *pSemIndex);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_DXIL_H_INCLUDED */
