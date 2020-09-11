#pragma once

// This is in a slightly awkward place to avoid having to
// redefine everywhere
#define SGX_WAMR_NATIVE_SYMBOL_COUNT 26
#define SGX_WAMR_WASI_SYMBOL_COUNT 6

#if(FAASM_SGX_ATTESTATION)
#include <sgx/sgx_wamr_attestation.h>
#endif

#if(FAASM_SGX_WHITELISTING)
#include <sgx/sgx_wamr_whitelisting.h>
#endif

#include <wasm_export.h>

typedef struct __sgx_wamr_tcs {
    wasm_module_t module;
    wasm_module_inst_t module_inst;
    uint8_t *wasm_opcode;

#if(FAASM_SGX_ATTESTATION)
    sgx_wamr_msg_t **response_ptr;
    _sgx_wamr_attestation_env_t env;
#endif

#if(FAASM_SGX_WHITELISTING)
    _sgx_wamr_whitelist_t *module_whitelist;
#endif

} _sgx_wamr_tcs_t;
