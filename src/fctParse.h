/*******************************************************************************
*   Factom Wallet
*   (c) 2018 The Factoid Authority
*            ledger@factoid.org
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/
#ifndef _FCT_PARSE_H_
#define _FCT_PARSE_H_

#include <stdbool.h>
#include <stdint.h>
#include "fctUtils.h"

#define MAX_BIP32_PATH 10
#define MAX_INPUT_ADDRESSES 10
#define MAX_OUTPUT_ADDRESSES 10
#define MAX_ECOUTPUT_ADDRESSES 10

#ifndef NULL
#define NULL 0
#endif



//header
typedef struct txContentHeader_t {
    uint8_t version;
    uint8_t *timestamp_ms;
    uint64_t fee;
    uint8_t inputcount;
    uint8_t outputcount;
    uint8_t ecpurchasecount;
} txContentHeader_t;

typedef struct txContentAddress_t {
    uint64_t value;
    int8_t  *rcdhash;
} txContentAddress_t;


typedef struct RCD_t {
    uint8_t *type;
    uint8_t *publickey;
} RCD_t;

typedef struct txSignedRCD_t {
    RCD_t RCD[256];
    uint8_t signature[256];
    uint8_t signature_len; 
} txSignedRCD_t;


typedef struct txContent_t {
    txContentHeader_t header;
    uint64_t fees;
    txContentAddress_t inputs[MAX_INPUT_ADDRESSES];
    txContentAddress_t outputs[MAX_OUTPUT_ADDRESSES];
    txContentAddress_t ecpurchase[MAX_ECOUTPUT_ADDRESSES];
} txContent_t;

parserStatus_e parseTx(uint8_t *data, uint32_t length, 
                       txContent_t *context);

#endif
