/*
 * Copyright (c) 2021, Cisco Systems, Inc.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://github.com/cisco/libacvp/LICENSE
 */



#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/ecdsa.h>
#include <openssl/ec.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/param_build.h>
#endif

#include "app_lcl.h"
#include "safe_lib.h"
#if OPENSSL_VERSION_NUMBER < 0x30000000L && defined ACVP_NO_RUNTIME
#include "app_fips_lcl.h" /* All regular OpenSSL headers must come before here */
#endif

static BIGNUM *ecdsa_group_Qx = NULL;
static BIGNUM *ecdsa_group_Qy = NULL;
static int ecdsa_current_tg = 0;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
static EVP_PKEY *group_pkey = NULL;
#else
static EC_KEY *ecdsa_group_key = NULL;
#endif

void app_ecdsa_cleanup(void) {
    if (ecdsa_group_Qx) BN_free(ecdsa_group_Qx);
    ecdsa_group_Qx = NULL;
    if (ecdsa_group_Qy) BN_free(ecdsa_group_Qy);
    ecdsa_group_Qy = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    if (group_pkey) EVP_PKEY_free(group_pkey);
    group_pkey = NULL;
#else
    if (ecdsa_group_key) EC_KEY_free(ecdsa_group_key);
    ecdsa_group_key = NULL;
#endif
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
int app_ecdsa_handler(ACVP_TEST_CASE *test_case) {
    int rv = 1, nid = NID_undef, key_size = 0;
    size_t sig_len = 0;
    const char *curve = NULL, *md = NULL;
    char *pub_key = NULL;
    unsigned char *sig = NULL, *sig_iter = NULL;
    ACVP_CIPHER mode;
    ACVP_SUB_ECDSA alg;
    ACVP_ECDSA_TC *tc = NULL;
    EVP_MD_CTX *sig_ctx = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;
    EVP_PKEY *pkey = NULL;
    OSSL_PARAM_BLD *pkey_pbld = NULL;
    OSSL_PARAM *params = NULL;
    ECDSA_SIG *sig_obj = NULL;
    BIGNUM *qx = NULL, *qy = NULL, *d = NULL;
    const BIGNUM *out_r = NULL, *out_s = NULL;
    BIGNUM *in_r = NULL, *in_s = NULL;
    if (!test_case) {
        printf("No test case found\n");
        return 1;
    }
    tc = test_case->tc.ecdsa;
    if (!tc) {
        printf("\nError: test case not found in ECDSA handler\n");
        return 1;
    }

    mode = tc->cipher;
    alg = acvp_get_ecdsa_alg(mode);
    if (alg == 0) {
        printf("Invalid cipher value");
        return 1;
    }

    nid = get_nid_for_curve(tc->curve);
    if (nid == NID_undef) {
        printf("Invalid curve provided for ECDSA\n");
        goto err;
    }
    curve = OSSL_EC_curve_nid2name(nid);
    if (!curve) {
        printf("Unable to lookup curve name for ECDSA\n");
        goto err;
    }

    if (mode == ACVP_ECDSA_SIGGEN || mode == ACVP_ECDSA_SIGVER) {
        md = get_md_string_for_hash_alg(tc->hash_alg);
        if (!md) {
            printf("Error getting hash alg from test case for ECDSA\n");
            goto err;
        }
    }

    switch (alg) {
    case ACVP_SUB_ECDSA_KEYGEN:
        pkey_ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (!pkey_ctx) {
            printf("Error initializing pkey CTX in ECDSA\n");
            goto err;
        }
        if (EVP_PKEY_keygen_init(pkey_ctx) != 1) {
            printf("Error initializing keygen in ECDSA keygen\n");
            goto err;
        }
        if (EVP_PKEY_CTX_set_group_name(pkey_ctx, curve) < 1) {
            printf("Error setting curve for ECDSA keygen\n");
            goto err;
        }
        EVP_PKEY_keygen(pkey_ctx, &pkey);
        if (!pkey) {
            printf("Error generating key in ECDSA keygen\n");
            goto err;
        }

        if (EVP_PKEY_get_bn_param(pkey, "priv", &d) == 1) {
            tc->d_len = BN_bn2bin(d, tc->d);
        } else {
            printf("Error getting 'd' in ECDSA keygen\n");
            goto err;
        }
        if (EVP_PKEY_get_bn_param(pkey, "qx", &qx) == 1) {
            tc->qx_len = BN_bn2bin(qx, tc->qx);
        } else {
            printf("Error getting 'qx' in ECDSA keygen\n");
            goto err;
        }
        if (EVP_PKEY_get_bn_param(pkey, "qy", &qy) == 1) {
            tc->qy_len = BN_bn2bin(qy, tc->qy);
        } else {
            printf("Error getting 'qy' in ECDSA keygen\n");
            goto err;
        }
        break;
    case ACVP_SUB_ECDSA_KEYVER:
        tc->ver_disposition = 0;

        pub_key = ec_point_to_pub_key(tc->qx, tc->qx_len, tc->qy, tc->qy_len, &key_size);
        if (!pub_key) {
            printf("Error generating pub key in ECDSA keyver\n");
            goto err;
        }

        pkey_pbld = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(pkey_pbld, "group", curve, 0);
        OSSL_PARAM_BLD_push_octet_string(pkey_pbld, "pub", pub_key, key_size);
        params = OSSL_PARAM_BLD_to_param(pkey_pbld);
        if (!params) {
            printf("Error generating parameters for pkey generation in RSA keygen\n");
        }

        pkey_ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (EVP_PKEY_fromdata_init(pkey_ctx) != 1) {
            printf("Error initializing fromdata in ECDSA keyver\n");
            goto err;
        }
        if (EVP_PKEY_fromdata(pkey_ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) == 1) {
            tc->ver_disposition = 1;
        }
        break;
    case ACVP_SUB_ECDSA_SIGGEN:
        if (ecdsa_current_tg != tc->tg_id) {
            ecdsa_current_tg = tc->tg_id;
            /* First, generate key for every test group */
            pkey_ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
            if (!pkey_ctx) {
                printf("Error creating pkey CTX in ECDSA siggen\n");
                goto err;
            }
            if (EVP_PKEY_keygen_init(pkey_ctx) != 1) {
                printf("Error initializing keygen in ECDSA siggen\n");
                goto err;
            }
            if (EVP_PKEY_CTX_set_group_name(pkey_ctx, curve) != 1) {
                printf("Error setting curve for ECDSA siggen\n");
                goto err;
            }
            if (EVP_PKEY_generate(pkey_ctx, &group_pkey) != 1) {
                printf("Error generating pkey in ECDSA siggen\n");
                goto err;
            }
            EVP_PKEY_get_bn_param(group_pkey, "qx", &ecdsa_group_Qx);
            EVP_PKEY_get_bn_param(group_pkey, "qy", &ecdsa_group_Qy);
            if (!ecdsa_group_Qx || !ecdsa_group_Qy) {
                printf("Error retrieving params from pkey in ECDSA siggen\n");
                goto err;
            }
        }
        /* Then, for each test case, generate a signature */
        sig_ctx = EVP_MD_CTX_new();
        if (!sig_ctx) {
            printf("Error initializing sign CTX for ECDSA siggen\n");
            goto err;
        }
        if (EVP_DigestSignInit_ex(sig_ctx, NULL, md, NULL, NULL, group_pkey, NULL) != 1) {
            printf("Error initializing signing for ECDSA siggen\n");
            goto err;
        }
        EVP_DigestSign(sig_ctx, NULL, &sig_len, tc->message, tc->msg_len);
        sig = calloc(sig_len, sizeof(char));
        if (!sig) {
            printf("Error allocating memory in ECDSA siggen\n");
            goto err;
        }
        sig_iter = sig; /* since d2i_ECDSA_SIG alters the pointer, we need to keep the original one for freeing */
        if (!sig) {
            printf("Error allocating memory for signature in ECDSA siggen\n");
            goto err;
        }
        if (EVP_DigestSign(sig_ctx, sig, &sig_len, tc->message, tc->msg_len) != 1) {
            printf("Error generating signature in ECDSA siggen\n");
            goto err;
        }

        /* Finally, extract R and S from signature */
        sig_obj = d2i_ECDSA_SIG(NULL, (const unsigned char **)&sig_iter, (long)sig_len);
        if (!sig_obj) {
            printf("Error creating signature object neeed to retrieve output in ECDSA siggen\n");
            goto err;
        }
        out_r = ECDSA_SIG_get0_r(sig_obj);
        out_s = ECDSA_SIG_get0_s(sig_obj);
        if (!out_r || !out_s) {
            printf("Error retrieving output values in ECDSA siggen\n");
            goto err;
        }
        /* and copy our values to the TC response */
        tc->r_len = BN_bn2bin(out_r, tc->r);
        tc->s_len = BN_bn2bin(out_s, tc->s);
        tc->qx_len = BN_bn2bin(ecdsa_group_Qx, tc->qx);
        tc->qy_len = BN_bn2bin(ecdsa_group_Qy, tc->qy);
        break;
    case ACVP_SUB_ECDSA_SIGVER:
        tc->ver_disposition = 0;

        pub_key = ec_point_to_pub_key(tc->qx, tc->qx_len, tc->qy, tc->qy_len, &key_size);
        if (!pub_key) {
            printf("Error generating pub key in ECDSA sigver\n");
            goto err;
        }

        pkey_pbld = OSSL_PARAM_BLD_new();
        OSSL_PARAM_BLD_push_utf8_string(pkey_pbld, "group", curve, 0);
        OSSL_PARAM_BLD_push_octet_string(pkey_pbld, "pub", pub_key, key_size);
        params = OSSL_PARAM_BLD_to_param(pkey_pbld);
        if (!params) {
            printf("Error generating parameters for pkey generation in RSA sigver\n");
        }

        pkey_ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
        if (!pkey_ctx) {
            printf("Error creating pkey ctx in ECDSA sigver\n");
            goto err;
        }
        if (EVP_PKEY_fromdata_init(pkey_ctx) != 1) {
            printf("Error initializing fromdata in ECDSA keyver\n");
            goto err;
        }
        if (EVP_PKEY_fromdata(pkey_ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) != 1) {
            printf("Error generating pkey from public key data in ECDSA sigver\n");
            goto err;
        }

        in_r = BN_bin2bn(tc->r, tc->r_len, NULL);
        in_s = BN_bin2bn(tc->s, tc->s_len, NULL);
        if (!in_r || !in_s) {
            printf("Error importing R or S in ECDSA sigver\n");
            goto err;
        }
        sig_obj = ECDSA_SIG_new();
        if (!sig_obj) {
            printf("Error creating signature object in ECDSA sigver\n");
            goto err;
        }
        if (ECDSA_SIG_set0(sig_obj, in_r, in_s) != 1) {
            printf("Error setting R and S values in ECDSA sigver\n");
            goto err;
        }

        sig_len = (size_t)i2d_ECDSA_SIG(sig_obj, &sig);
        sig_ctx = EVP_MD_CTX_new();
        if (!sig_ctx) {
            printf("Error initializing sign CTX for ECDSA sigver\n");
            goto err;
        }            

        if (EVP_DigestVerifyInit_ex(sig_ctx, NULL, md, NULL, NULL, pkey, NULL) != 1) {
            printf("Error initializing signing for ECDSA sigver\n");
            goto err;
        }
        if (EVP_DigestVerify(sig_ctx, sig, sig_len, tc->message, tc->msg_len) == 1) {
            tc->ver_disposition = 1;
        }
        break;
    default:
        printf("Invalid ECDSA alg in test case\n");
        goto err;
    }
    rv = 0;
err:
    if (qx) BN_free(qx);
    if (qy) BN_free(qy);
    if (d) BN_free(d);
    if (pub_key) free(pub_key);
    if (sig) free(sig);
    if (pkey_pbld) OSSL_PARAM_BLD_free(pkey_pbld);
    if (params) OSSL_PARAM_free(params);
    if (sig_obj) ECDSA_SIG_free(sig_obj);
    if (pkey) EVP_PKEY_free(pkey);
    if (sig_ctx) EVP_MD_CTX_free(sig_ctx);
    if (pkey_ctx) EVP_PKEY_CTX_free(pkey_ctx);
    return rv;
}

#elif defined ACVP_NO_RUNTIME

static int ec_get_pubkey(EC_KEY *key, BIGNUM *x, BIGNUM *y) {
    const EC_POINT *pt;
    const EC_GROUP *grp;
    const EC_METHOD *meth;
    int rv = 0;
    BN_CTX *ctx;

    ctx = BN_CTX_new();
    if (!ctx) return 0;

    grp = EC_KEY_get0_group(key);
    if (!grp) goto end;

    pt = EC_KEY_get0_public_key(key);
    if (!pt) goto end;

    meth = EC_GROUP_method_of(grp);
    if (EC_METHOD_get_field_type(meth) == NID_X9_62_prime_field) {
        rv = EC_POINT_get_affine_coordinates_GFp(grp, pt, x, y, ctx);
    } else {
        rv = EC_POINT_get_affine_coordinates_GF2m(grp, pt, x, y, ctx);
    }

end:
    if (ctx) BN_CTX_free(ctx);
    return rv;
}

int app_ecdsa_handler(ACVP_TEST_CASE *test_case) {
    ACVP_ECDSA_TC    *tc;
    int rv = 1;
    ACVP_CIPHER mode;
    const EVP_MD *md = NULL;
    ECDSA_SIG *sig = NULL;

    int nid = NID_undef, rc = 0, msg_len = 0;
    BIGNUM *Qx = NULL, *Qy = NULL;
    BIGNUM *r = NULL, *s = NULL;
    const BIGNUM *a = NULL, *b = NULL;
    const BIGNUM *d = NULL;
    EC_KEY *key = NULL;
    ACVP_SUB_ECDSA alg;

    if (!test_case) {
        printf("No test case found\n");
        return 1;
    }
    tc = test_case->tc.ecdsa;
    if (!tc) {
        printf("\nError: test case not found in ECDSA handler\n");
        return 1;
    }
    mode = tc->cipher;

    if (mode == ACVP_ECDSA_SIGGEN || mode == ACVP_ECDSA_SIGVER) {
        md = get_md_for_hash_alg(tc->hash_alg);
        if (!md) {
            printf("Error getting MD for ECDSA\n");
            goto err;
        }
    }

    nid = get_nid_for_curve(tc->curve);
    if (nid == NID_undef) {
        printf("Invalid curve given for ECDSA\n");
        goto err;
    }

    alg = acvp_get_ecdsa_alg(mode);
    if (alg == 0) {
        printf("Invalid cipher value");
        return 1;
    }

    switch (alg) {
    case ACVP_SUB_ECDSA_KEYGEN:
        Qx = FIPS_bn_new();
        Qy = FIPS_bn_new();
        if (!Qx || !Qy) {
            printf("Error BIGNUM malloc\n");
            goto err;
        }

        key = EC_KEY_new_by_curve_name(nid);
        if (!key) {
            printf("Failed to instantiate ECDSA key\n");
            goto err;
        }

        if (!EC_KEY_generate_key(key)) {
            printf("Error generating ECDSA key\n");
            goto err;
        }

        if (!ec_get_pubkey(key, Qx, Qy)) {
            printf("Error getting ECDSA key attributes\n");
            goto err;
        }

        d = EC_KEY_get0_private_key(key);

        tc->qx_len = BN_bn2bin(Qx, tc->qx);
        tc->qy_len = BN_bn2bin(Qy, tc->qy);
        tc->d_len = BN_bn2bin(d, tc->d);
        break;
    case ACVP_SUB_ECDSA_KEYVER:
        Qx = FIPS_bn_new();
        Qy = FIPS_bn_new();
        if (!tc->qx || !tc->qy) {
            printf("missing qx or qy: ecdsa keyver\n");
            goto err;
        }
        BN_bin2bn(tc->qx, tc->qx_len, Qx);
        BN_bin2bn(tc->qy, tc->qy_len, Qy);
        if (!Qx || !Qy) {
            printf("Error BIGNUM conversion\n");
            goto err;
        }

        key = EC_KEY_new_by_curve_name(nid);
        if (!key) {
            printf("Failed to instantiate ECDSA key\n");
            goto err;
        }

        if (EC_KEY_set_public_key_affine_coordinates(key, Qx, Qy) == 1) {
            tc->ver_disposition = ACVP_TEST_DISPOSITION_PASS;
        } else {
            tc->ver_disposition = ACVP_TEST_DISPOSITION_FAIL;
        }
        break;
    case ACVP_SUB_ECDSA_SIGGEN:
        if (ecdsa_current_tg != tc->tg_id) {
            ecdsa_current_tg = tc->tg_id;

            /* Free the group objects before re-allocation */
            if (ecdsa_group_key) EC_KEY_free(ecdsa_group_key);
            ecdsa_group_key = NULL;
            if (ecdsa_group_Qx) BN_free(ecdsa_group_Qx);
            ecdsa_group_Qx = NULL;
            if (ecdsa_group_Qy) BN_free(ecdsa_group_Qy);
            ecdsa_group_Qy = NULL;

            ecdsa_group_Qx = FIPS_bn_new();
            ecdsa_group_Qy = FIPS_bn_new();
            if (!ecdsa_group_Qx || !ecdsa_group_Qy) {
                printf("Error BIGNUM malloc\n");
                goto err;
            }
            ecdsa_group_key = EC_KEY_new_by_curve_name(nid);
            if (!ecdsa_group_key) {
                printf("Failed to instantiate ECDSA key\n");
                goto err;
            }

            if (!EC_KEY_generate_key(ecdsa_group_key)) {
                printf("Error generating ECDSA key\n");
                goto err;
            }

            if (!ec_get_pubkey(ecdsa_group_key, ecdsa_group_Qx, ecdsa_group_Qy)) {
                printf("Error getting ECDSA key attributes\n");
                goto err;
            }
        }
        msg_len = tc->msg_len;
        if (!tc->message) {
            printf("ecdsa siggen missing msg\n");
            goto err;
        }
        sig = FIPS_ecdsa_sign_md(ecdsa_group_key, tc->message, msg_len, md);

        if (!sig) {
            printf("Error signing message\n");
            goto err;
        }

        ECDSA_SIG_get0(sig, &a, &b);
        r = BN_dup(a);
        s = BN_dup(b);

        tc->qx_len = BN_bn2bin(ecdsa_group_Qx, tc->qx);
        tc->qy_len = BN_bn2bin(ecdsa_group_Qy, tc->qy);
        tc->r_len = BN_bn2bin(r, tc->r);
        tc->s_len = BN_bn2bin(s, tc->s);
        BN_free(s);
        BN_free(r);
        break;
    case ACVP_SUB_ECDSA_SIGVER:
        if (!tc->message) {
            printf("missing sigver message - nothing to verify\n");
            goto err;
        }
        if (!tc->r) {
            printf("missing r ecdsa sigver\n");
            goto err;
        }
        if (!tc->s) {
            printf("missing s ecdsa sigver\n");
            goto err;
        }
        sig = ECDSA_SIG_new();
        if (!sig) {
            printf("Error generating ecdsa signature\n");
            goto err;
        }

        r = FIPS_bn_new();
        s = FIPS_bn_new();
        ECDSA_SIG_set0(sig, r, s);

        Qx = FIPS_bn_new();
        Qy = FIPS_bn_new();

        if (!Qx || !Qy) {
            printf("Error BIGNUM conversion\n");
            goto err;
        }
        BN_bin2bn(tc->qx, tc->qx_len, Qx);
        BN_bin2bn(tc->qy, tc->qy_len, Qy);

        if (!r || !s) {
            printf("Error BIGNUM conversion\n");
            goto err;
        }
        BN_bin2bn(tc->r, tc->r_len, r);
        BN_bin2bn(tc->s, tc->s_len, s);

        key = EC_KEY_new_by_curve_name(nid);
        if (!key) {
            printf("Failed to instantiate ECDSA key\n");
            goto err;
        }

        rc = EC_KEY_set_public_key_affine_coordinates(key, Qx, Qy);
        if (rc != 1) {
            printf("Error setting ECDSA coordinates\n");
            goto points_err;
        }

        if (!tc->message) {
            printf("ecdsa siggen missing msg\n");
            goto err;
        }
        if (FIPS_ecdsa_verify_md(key, tc->message, tc->msg_len, md, sig) == 1) {
            tc->ver_disposition = ACVP_TEST_DISPOSITION_PASS;
        } else {
            tc->ver_disposition = ACVP_TEST_DISPOSITION_FAIL;
        }
points_err:
        break;
    default:
        printf("Unsupported ECDSA mode\n");
        break;
    }
    rv = 0;

err:
    if (sig) FIPS_ecdsa_sig_free(sig);
    if (Qx) FIPS_bn_free(Qx);
    if (Qy) FIPS_bn_free(Qy);
    if (key) EC_KEY_free(key);
    return rv;
}
#else
int app_ecdsa_handler(ACVP_TEST_CASE *test_case) {
    if (!test_case) {
        return -1;
    }
    return 1;
}
#endif // ACVP_NO_RUNTIME

