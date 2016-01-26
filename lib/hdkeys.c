/* Copyright 2016 BitPay, Inc.
 * Copyright 2016 Duncan Tebbs
 * Distributed under the MIT/X11 software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 */

#include <ccoin/hdkeys.h>
#include <ccoin/buffer.h>
#include <ccoin/serialize.h>
#include <ccoin/util.h>

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/ripemd.h>

#define MAIN_PUBLIC 0x0488B21E
#define MAIN_PRIVATE 0x0488ADE4
#define TEST_PUBLIC 0x043587CF
#define TEST_PRIVATE 0x04358394

static int EC_KEY_add_priv(EC_KEY *out_key, const EC_KEY *key,
			   const BIGNUM *tweak)
{
	// Add the point generated by tweak to key.  If key has a private
	// key, out_key must also have.

	BN_CTX *ctx = BN_CTX_new();
	if (NULL == ctx) {
		return false;
	}

	int ok = 0;
	const EC_GROUP *group = EC_KEY_get0_group(key);
	BIGNUM *order = BN_new();
	if (!EC_GROUP_get_order(group, order, ctx)) {
		goto free_ctx;
	}

	const BIGNUM *key_secret = EC_KEY_get0_private_key(key);
	if (NULL == key_secret) {
		// Use EC_POINTS: tweak * generator + 1 * key_pub

		const EC_POINT *key_pub = EC_KEY_get0_public_key(key);
		EC_POINT *new_pub = EC_POINT_new(group);
		if (NULL == new_pub) {
			goto free_order;
		}

		if (EC_POINT_mul(group, new_pub, tweak, key_pub, BN_value_one(), ctx)) {
			EC_KEY_set_public_key(out_key, new_pub);
			EC_KEY_set_conv_form(out_key, POINT_CONVERSION_COMPRESSED);
			ok = 1;
		}

		EC_POINT_free(new_pub);
	} else {
		// Use bignums

		BIGNUM *new_secret = BN_new();
		if (!BN_mod_add(new_secret, key_secret, tweak, order, ctx)) {
			goto free_order;
		}

		EC_POINT *new_pub = EC_POINT_new(group);
		if (NULL != new_pub) {
			if (EC_POINT_mul(group, new_pub, new_secret, NULL, NULL, ctx)) {
				EC_KEY_set_private_key(out_key, new_secret);
				EC_KEY_set_public_key(out_key, new_pub);
				if (EC_KEY_check_key(out_key)) {
					EC_KEY_set_conv_form(out_key, POINT_CONVERSION_COMPRESSED);
					ok = 1;
				}
			}

			EC_POINT_free(new_pub);
		}

		BN_clear_free(new_secret);
	}

free_order:
	BN_clear_free(order);
free_ctx:
	BN_CTX_free(ctx);
	return ok;
}

bool hd_extended_key_init(struct hd_extended_key *ek)
{
	if (bp_key_init(&ek->key)) {
		memset(ek->chaincode.data, 0, sizeof(ek->chaincode.data));
		ek->index = 0;
		ek->version = 0;
		memset(ek->parent_fingerprint, 0, 4);
		ek->depth = 0;
		return true;
	}
	return false;
}

void hd_extended_key_free(struct hd_extended_key *ek)
{
	bp_key_free(&ek->key);
}

bool hd_extended_key_deser(struct hd_extended_key *ek, const void *_data,
			   size_t len)
{
	if (78 != len && 82 != len) return false;

	struct const_buffer buf = { _data, len };
	uint32_t version;

	if (!deser_u32(&version, &buf)) return false;
	ek->version = version = be32toh(version);
	if (!deser_bytes(&ek->depth, &buf, 1)) return false;
	if (!deser_bytes(&ek->parent_fingerprint, &buf, 4)) return false;
	if (!deser_u32(&ek->index, &buf)) return false;
	ek->index = be32toh(ek->index);
	if (!deser_bytes(&ek->chaincode.data, &buf, 32)) return false;

	if (MAIN_PUBLIC == version || TEST_PUBLIC == version) {
		if (bp_pubkey_set(&ek->key, buf.p, 33)) {
			return true;
		}
	} else if (MAIN_PRIVATE == version || TEST_PRIVATE == version) {
		uint8_t zero;
		if (deser_bytes(&zero, &buf, 1) && 0 == zero) {
			if (bp_key_secret_set(&ek->key, buf.p, 32)) {
				return true;
			}
		}
	}

	return false;
}

static void hd_extended_key_ser_base(const struct hd_extended_key *ek,
				     cstring *s, uint32_t version)
{
	ser_u32(s, htobe32(version));
	ser_bytes(s, &ek->depth, 1);
	ser_bytes(s, &ek->parent_fingerprint, 4);
	ser_u32(s, htobe32(ek->index));
	ser_bytes(s, &ek->chaincode, 32);
}

bool hd_extended_key_ser_pub(const struct hd_extended_key *ek, cstring *s)
{
	hd_extended_key_ser_base(ek, s, MAIN_PUBLIC);

	void *pub;
	size_t pub_len;
	if (bp_pubkey_get(&ek->key, &pub, &pub_len) && 33 == pub_len) {
		ser_bytes(s, pub, 33);
		free(pub);
		return true;
	}
	return false;
}

bool hd_extended_key_ser_priv(const struct hd_extended_key *ek, cstring *s)
{
	hd_extended_key_ser_base(ek, s, MAIN_PRIVATE);

	const uint8_t zero = 0;
	ser_bytes(s, &zero, 1);
	return bp_key_secret_get(s->str + s->len, 32, &ek->key);
}

bool hd_extended_key_generate_master(struct hd_extended_key *ek,
				     const void *seed, size_t seed_len)
{
	static const uint8_t key[12] = "Bitcoin seed";
	uint8_t I[64];
	HMAC(EVP_sha512(), key, (int)sizeof(key), (const uint8_t *)seed,
	     (uint32_t)seed_len, &I[0], NULL);

	if (bp_key_secret_set(&ek->key, I, 32)) {
		memcpy(ek->chaincode.data, &I[32], 32);
		ek->index = 0;
		ek->version = MAIN_PRIVATE; // get's set public / private during
		memset(ek->parent_fingerprint, 0, 4);
		ek->depth = 0;

		return true;
	}

	return false;
}

bool hd_extended_key_generate_child(const struct hd_extended_key *parent,
				    uint32_t index,
				    struct hd_extended_key *out_child)
{
	bool result = false;

	// Get parent public key

	void *parent_pub = NULL;
	size_t parent_pub_len = 0;
	if (!bp_pubkey_get(&parent->key, &parent_pub, &parent_pub_len)) {
		return false;
	}
	if (33 != parent_pub_len) {
		goto free_parent_pub;
	}

	uint8_t data[33 + sizeof(uint32_t)];
	if (0 != (0x80000000 & index)) {
		if (!bp_key_secret_get(&data[1], 32, &parent->key)) {
			goto free_parent_pub;
		}

		data[0] = 0;
	} else {
		memcpy(&data[0], parent_pub, parent_pub_len);
	}

	const uint32_t indexBE = htobe32(index);
	memcpy(&data[33], &indexBE, sizeof(uint32_t));

	uint8_t I[64];
	if (NULL == HMAC(EVP_sha512(), parent->chaincode.data,
			 (int)sizeof(parent->chaincode.data), data,
			 (int)sizeof(data), &I[0], NULL)) {
		goto free_parent_pub;
	}

	BIGNUM k_IL;
	BN_init(&k_IL);
	if (NULL == BN_bin2bn(I, 32, &k_IL)) {
		goto free_parent_pub;
	}

	if (!EC_KEY_add_priv(out_child->key.k, parent->key.k, &k_IL)) {
		goto free_kIL;
	}

	uint8_t md160[RIPEMD160_DIGEST_LENGTH];
	bu_Hash160(md160, parent_pub, parent_pub_len);

	memcpy(out_child->chaincode.data, &I[32], 32);
	out_child->index = index;
	out_child->version = parent->version;
	memcpy(out_child->parent_fingerprint, md160, 4);
	out_child->depth = parent->depth + 1;
	result = true;

free_kIL:
	BN_clear_free(&k_IL);
free_parent_pub:
	free(parent_pub);

	return result;
}
