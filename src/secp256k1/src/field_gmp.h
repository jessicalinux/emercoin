/**********************************************************************
 * Copyright (c) 2013, 2014 Pieter Wuille                             *
 * Distributed under the GPL3 software license, see the accompanying   *
 * file COPYING or http://www.gnu.org/licenses/gpl.html.*
 **********************************************************************/

#ifndef _SECP256K1_FIELD_REPR_
#define _SECP256K1_FIELD_REPR_

#include <gmp.h>

#define FIELD_LIMBS ((256 + GMP_NUMB_BITS - 1) / GMP_NUMB_BITS)

typedef struct {
    mp_limb_t n[FIELD_LIMBS+1];
} secp256k1_fe_t;

#endif
