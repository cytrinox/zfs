/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2016, Datto, Inc. All rights reserved.
 */

#include <sys/zio_crypt.h>
#include <sys/dmu.h>
#include <sys/dnode.h>
#include <sys/fs/zfs.h>
#include <sys/zio.h>
#include <sys/zil.h>

/*
 * This file is responsible for handling all of the details of generating
 * encryption parameters and performing encryption.
 *
 * BLOCK ENCRYPTION PARAMETERS:
 * Encryption Algorithm (crypt):
 * The encryption algorithm and mode we are going to use. We
 * currently support AES-GCM and AES-CCM in 128, 192, and 256 bits.
 *
 * Plaintext:
 * The unencrypted data that we want to encrypt
 *
 * Initialization Vector (IV):
 * An initialization vector for the encryption algorithms. This is
 * used to "tweak" the encryption algorithms so that equivalent blocks of
 * data are encrypted into different ciphertext outputs. Different modes
 * of encryption have different requirements for the IV. AES-GCM and AES-CCM
 * require that an IV is never reused with the same encryption key. This
 * value is stored unencrypted and must simply be provided to the decryption
 * function. We use a 96 bit IV (as reccommended by NIST). For non-dedup blocks
 * we derive the IV from a hash of DVA[0] and the birth txg and a randomly
 * generated salt (described blow). Normally, DVA[0] + birth txg should be
 * unique to a pool, but ZFS is susceptible to being "rewound" slightly due
 * to its copy-on-write mechanisms, so we add in the salt for extra security.
 *
 * Master key:
 * This is the most important secret data of an encrypted dataset. It is used
 * along with the salt to generate that actual encryption keys via HKDF. We
 * do not use the master key to encrypt any data because there are theoretical
 * limits on how much data can actually be safely encrypted with any encryption
 * mode. The master key is stored encrypted on disk with the user's key. It's
 * length is determined by the encryption algorithm. For details on how this is
 * stored see the block comment in dsl_crypt.c
 *
 * Salt:
 * Used as an input to the HKDF function, along with the master key. We use a
 * 64 bit salt, stored unencrypted in blk_fill. Any given salt can be used for
 * encrypting many blocks, so we cache the current salt and the associated
 * derived key in zio_crypt_t so we do not need to derive it again needlessly.
 *
 * Encryption Key:
 * A secret binary key, generated from an HKDF function used to encrypt and
 * decrypt data.
 *
 * Message Authenication Code (MAC)
 * The MAC is an output of authenticated encryption modes suce ahes AES-GCM and
 * AES-CCM. It's purpose is to ensure that an attacker cannot modify encrypted
 * data on disk and return garbage to the application. Effectively, it is a
 * checksum that can not be reproduced by an attacker. We store the MAC in the
 * first 128 bits of blk_cksum, leaving the second 128 bits for a truncated
 * regular checksum of the ciphertext which can be used for scrubbing.
 *
 *
 * ZIL ENCRYPTION:
 * ZIL blocks have their bp written to disk ahead of the associated data, so we
 * cannot store encyrption paramaters there as we normally do. For these blocks
 * the MAC is stored in the zil_chain_t header (in zc_mac) in a previously
 * unused 8 bytes. The salt is generated for the block on bp allocation. The IV
 * cannot be generated using the birth txg becuase this will be 0 on encryption
 * but it will have a real txg on decryption. In its place we add a hash of the
 * bookmark into the IV generation. This bookmark should has a unique sequence
 * number so it should be suitable for creating a unique IV.
 *
 * CONSIDERATIONS FOR DEDUP:
 * In order for dedup to work, we need to ensure that the ciphertext checksum
 * and MAC are quivalent for equivalent plaintexts. This requires using the
 * same IV and encryption key for equivalent blocks of plaindata. Normally,
 * one should never reuse an IV with the same encryption key or else AES-GCM
 * and AES-CCM can both actually leak the plaintext of both blocks. In this
 * case, however, since we are using the same plaindata as well all that we end
 * up with is a duplicate of the original data we already had. As a result,
 * an attacker with read access to the raw disk will be able to tell which
 * blocks are the same but this information is already given away by dedup
 * anyway. In order to get the same IVs and encryption keys for equivalent
 * blocks of data we use a HMAC of the plaindata. We use an HMAC here so there
 * is never a reproducible checksum of the plaintext available to the attacker.
 * The HMAC key is kept alongside the master key, encrypted on disk. The first
 * 64 bits are used in place of the salt, and the next 96 bits replace the IV
 * generated from DVA[0] + birth txg + the salt. At decryption time we will not
 * be able to perform an HMAC of the plaindata since we won't have it. In this
 * case we store the IV in DVA[2]. This means that an encrypted, dedup'd block
 * cannot have more than 2 copies. If this becomes a problem in the future, the
 * dedup table itself can be leveraged to hold additional copies.
 *
 * L2ARC ENCRYPTION:
 * L2ARC block encryption works very similarly to normal block encryption.
 * The main difference is that the (poolwide) L2ARC encryption key, and MAC
 * are kept only in memory and the IV is generated from fields in the L2ARC
 * buffer header. Only the ciphertext is stored on disk. This means that once
 * the system is powered off the encryption key is no longer accessible,
 * leaving the persisted ciphertext completely lost.
 */

zio_crypt_info_t zio_crypt_table[ZIO_CRYPT_FUNCTIONS] = {
	{"",			ZC_TYPE_NONE,	0,  "inherit"},
	{SUN_CKM_AES_CCM,	ZC_TYPE_CCM,	32, "on"},
	{"",			ZC_TYPE_NONE,	0,  "off"},
	{SUN_CKM_AES_CCM,	ZC_TYPE_CCM,	16, "aes-128-ccm"},
	{SUN_CKM_AES_CCM,	ZC_TYPE_CCM,	24, "aes-192-ccm"},
	{SUN_CKM_AES_CCM,	ZC_TYPE_CCM,	32, "aes-256-ccm"},
	{SUN_CKM_AES_GCM,	ZC_TYPE_GCM,	16, "aes-128-gcm"},
	{SUN_CKM_AES_GCM,	ZC_TYPE_GCM,	24, "aes-192-gcm"},
	{SUN_CKM_AES_GCM,	ZC_TYPE_GCM,	32, "aes-256-gcm"}
};

void
l2arc_crypt_key_destroy(l2arc_crypt_key_t *key)
{
	crypto_destroy_ctx_template(key->l2ck_ctx_tmpl);

	if (key->l2ck_key.ck_data) {
		bzero(key->l2ck_key.ck_data,
		    BITS_TO_BYTES(key->l2ck_key.ck_length));
		kmem_free(key->l2ck_key.ck_data,
		    BITS_TO_BYTES(key->l2ck_key.ck_length));
	}
}

int
l2arc_crypt_key_init(l2arc_crypt_key_t *key)
{
	int ret;
	crypto_mechanism_t mech;
	uint64_t keydata_len, crypt = L2ARC_DEFAULT_CRYPT;

	key->l2ck_crypt = crypt;

	/* get the key length from the crypt table */
	keydata_len = zio_crypt_table[crypt].ci_keylen;

	/* allocate the key data's new buffer */
	key->l2ck_key.ck_data = kmem_alloc(keydata_len, KM_SLEEP);
	if (!key->l2ck_key.ck_data) {
		ret = ENOMEM;
		goto error;
	}

	/* set values for the key */
	key->l2ck_key.ck_format = CRYPTO_KEY_RAW;
	key->l2ck_key.ck_length = BYTES_TO_BITS(keydata_len);

	/*
	 * Create the data. We can use pseudo random bytes here
	 * because this key will not persist through reboots.
	 */
	ret = random_get_pseudo_bytes(key->l2ck_key.ck_data, keydata_len);
	if (ret)
		goto error;

	/* create the key's context template */
	mech.cm_type = crypto_mech2id(zio_crypt_table[crypt].ci_mechname);
	ret = crypto_create_ctx_template(&mech, &key->l2ck_key,
	    &key->l2ck_ctx_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->l2ck_ctx_tmpl = NULL;

	return (0);

error:
	l2arc_crypt_key_destroy(key);
	return (ret);
}

static int
hkdf_sha256_extract(uint8_t *salt, uint_t salt_len, uint8_t *key_material,
    uint_t km_len, uint8_t *out_buf)
{
	int ret;
	crypto_mechanism_t mech;
	crypto_key_t key;
	crypto_data_t input_cd, output_cd;

	/* initialize sha 256 hmac mechanism */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	/* initialize the salt as a crypto key */
	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_length = BYTES_TO_BITS(salt_len);
	key.ck_data = salt;

	/* initialize crypto data for the input and output data */
	input_cd.cd_format = CRYPTO_DATA_RAW;
	input_cd.cd_offset = 0;
	input_cd.cd_length = km_len;
	input_cd.cd_raw.iov_base = (char *)key_material;
	input_cd.cd_raw.iov_len = km_len;

	output_cd.cd_format = CRYPTO_DATA_RAW;
	output_cd.cd_offset = 0;
	output_cd.cd_length = SHA_256_DIGEST_LEN;
	output_cd.cd_raw.iov_base = (char *)out_buf;
	output_cd.cd_raw.iov_len = SHA_256_DIGEST_LEN;

	ret = crypto_mac(&mech, &input_cd, &key, NULL, &output_cd, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	return (0);

error:
	return (ret);
}

static int
hkdf_sha256_expand(uint8_t *extract_key, uint8_t *info, uint_t info_len,
    uint8_t *out_buf, uint_t out_len)
{
	int ret;
	crypto_mechanism_t mech;
	crypto_context_t ctx;
	crypto_key_t key;
	crypto_data_t T_cd, info_cd, c_cd;
	uint_t i, T_len = 0, pos = 0;
	uint_t c;
	uint_t N = (out_len + SHA_256_DIGEST_LEN) / SHA_256_DIGEST_LEN;
	uint8_t T[SHA_256_DIGEST_LEN];

	if (N > 255)
		return (SET_ERROR(EINVAL));

	/* initialize sha 256 hmac mechanism */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	/* initialize the salt as a crypto key */
	key.ck_format = CRYPTO_KEY_RAW;
	key.ck_length = BYTES_TO_BITS(SHA_256_DIGEST_LEN);
	key.ck_data = extract_key;

	/* initialize crypto data for the input and output data */
	T_cd.cd_format = CRYPTO_DATA_RAW;
	T_cd.cd_offset = 0;
	T_cd.cd_raw.iov_base = (char *)T;

	c_cd.cd_format = CRYPTO_DATA_RAW;
	c_cd.cd_offset = 0;
	c_cd.cd_length = 1;
	c_cd.cd_raw.iov_base = (char *)&c;
	c_cd.cd_raw.iov_len = 1;

	info_cd.cd_format = CRYPTO_DATA_RAW;
	info_cd.cd_offset = 0;
	info_cd.cd_length = info_len;
	info_cd.cd_raw.iov_base = (char *)info;
	info_cd.cd_raw.iov_len = info_len;

	for (i = 1; i <= N; i++) {
		c = i;

		T_cd.cd_length = T_len;
		T_cd.cd_raw.iov_len = T_len;

		ret = crypto_mac_init(&mech, &key, NULL, &ctx, NULL);
		if (ret != CRYPTO_SUCCESS) {
			ret = SET_ERROR(EIO);
			goto error;
		}

		ret = crypto_mac_update(ctx, &T_cd, NULL);
		if (ret != CRYPTO_SUCCESS) {
			ret = SET_ERROR(EIO);
			goto error;
		}

		ret = crypto_mac_update(ctx, &info_cd, NULL);
		if (ret != CRYPTO_SUCCESS) {
			ret = SET_ERROR(EIO);
			goto error;
		}

		ret = crypto_mac_update(ctx, &c_cd, NULL);
		if (ret != CRYPTO_SUCCESS) {
			ret = SET_ERROR(EIO);
			goto error;
		}

		T_len = SHA_256_DIGEST_LEN;
		T_cd.cd_length = T_len;
		T_cd.cd_raw.iov_len = T_len;

		ret = crypto_mac_final(ctx, &T_cd, NULL);
		if (ret != CRYPTO_SUCCESS) {
			ret = SET_ERROR(EIO);
			goto error;
		}

		bcopy(T, out_buf + pos,
		    (i != N) ? SHA_256_DIGEST_LEN : (out_len - pos));
		pos += SHA_256_DIGEST_LEN;
	}

	return (0);

error:
	return (ret);
}

/*
 * HKDF is designed to be a relatively fast function for deriving keys from a
 * master key + a salt. We use this function to generate new encryption keys
 * so as to avoid hitting the cryptographic limits of the underlying
 * encryption modes. Note that, for the sake of deriving encryption keys, the
 * info parameter is called the "salt" everywhere else in the code.
 */
static int
hkdf_sha256(uint8_t *key_material, uint_t km_len, uint8_t *salt,
    uint_t salt_len, uint8_t *info, uint_t info_len, uint8_t *output_key,
    uint_t out_len)
{
	int ret;
	uint8_t extract_key[SHA_256_DIGEST_LEN];

	ret = hkdf_sha256_extract(salt, salt_len, key_material, km_len,
	    extract_key);
	if (ret)
		goto error;

	ret = hkdf_sha256_expand(extract_key, info, info_len, output_key,
	    out_len);
	if (ret)
		goto error;

	return (0);

error:
	return (ret);
}

void
zio_crypt_key_destroy(zio_crypt_key_t *key)
{
	rw_destroy(&key->zk_salt_lock);

	/* free crypto templates */
	crypto_destroy_ctx_template(key->zk_current_tmpl);
	crypto_destroy_ctx_template(key->zk_hmac_tmpl);

	/* zero out sensitive data */
	bzero(key, sizeof (zio_crypt_key_t));
}

int
zio_crypt_key_init(uint64_t crypt, zio_crypt_key_t *key)
{
	int ret;
	crypto_mechanism_t mech;
	uint_t keydata_len;

	ASSERT(key != NULL);
	ASSERT3U(crypt, <, ZIO_CRYPT_FUNCTIONS);

	keydata_len = zio_crypt_table[crypt].ci_keylen;

	/* fill keydata buffers and salt with random data */
	ret = random_get_bytes(key->zk_master_keydata, keydata_len);
	if (ret)
		goto error;

	ret = random_get_bytes(key->zk_hmac_keydata, HMAC_SHA256_KEYLEN);
	if (ret)
		goto error;

	ret = random_get_bytes(key->zk_salt, DATA_SALT_LEN);
	if (ret)
		goto error;

	/* derive the current key from the master key */
	ret = hkdf_sha256(key->zk_master_keydata, keydata_len, NULL, 0,
	    key->zk_salt, DATA_SALT_LEN, key->zk_current_keydata, keydata_len);
	if (ret)
		goto error;

	/* initialize keys for the ICP */
	key->zk_current_key.ck_format = CRYPTO_KEY_RAW;
	key->zk_current_key.ck_data = key->zk_current_keydata;
	key->zk_current_key.ck_length = BYTES_TO_BITS(keydata_len);

	key->zk_hmac_key.ck_format = CRYPTO_KEY_RAW;
	key->zk_hmac_key.ck_data = &key->zk_hmac_key;
	key->zk_hmac_key.ck_length = BYTES_TO_BITS(HMAC_SHA256_KEYLEN);

	/*
	 * Initialize the crypto templates. It's ok if this fails because
	 * this is just an optimization.
	 */
	mech.cm_type = crypto_mech2id(zio_crypt_table[crypt].ci_mechname);
	ret = crypto_create_ctx_template(&mech, &key->zk_current_key,
	    &key->zk_current_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->zk_current_tmpl = NULL;

	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	ret = crypto_create_ctx_template(&mech, &key->zk_hmac_key,
	    &key->zk_hmac_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->zk_hmac_tmpl = NULL;

	key->zk_crypt = crypt;
	key->zk_salt_count = 0;
	rw_init(&key->zk_salt_lock, NULL, RW_DEFAULT, NULL);

	return (0);

error:
	zio_crypt_key_destroy(key);
	return (ret);
}

static int
zio_crypt_key_change_salt(zio_crypt_key_t *key)
{
	int ret;
	uint8_t salt[DATA_SALT_LEN];
	crypto_mechanism_t mech;
	uint_t keydata_len = zio_crypt_table[key->zk_crypt].ci_keylen;

	/* generate a new salt */
	ret = random_get_bytes(salt, DATA_SALT_LEN);
	if (ret)
		goto error;

	rw_enter(&key->zk_salt_lock, RW_WRITER);

	/* derive the current key from the master key and the new salt */
	ret = hkdf_sha256(key->zk_master_keydata, keydata_len, NULL, 0,
	    salt, DATA_SALT_LEN, key->zk_current_keydata, keydata_len);
	if (ret)
		goto error_unlock;

	/* assign the salt and reset the usage count */
	bcopy(salt, key->zk_salt, DATA_SALT_LEN);
	key->zk_salt_count = 0;

	/* destroy the old context template and create the new one */
	crypto_destroy_ctx_template(key->zk_current_tmpl);
	ret = crypto_create_ctx_template(&mech, &key->zk_current_key,
	    &key->zk_current_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->zk_current_tmpl = NULL;

	rw_exit(&key->zk_salt_lock);

	return (0);

error_unlock:
	rw_exit(&key->zk_salt_lock);
error:
	return (ret);
}

/* See comment above ZIO_CRYPT_MAX_SALT_USAGE definition for details */
int
zio_crypt_key_get_salt(zio_crypt_key_t *key, uint8_t *salt)
{
	int ret;
	boolean_t salt_change;

	rw_enter(&key->zk_salt_lock, RW_READER);

	bcopy(key->zk_salt, salt, DATA_SALT_LEN);
	salt_change = (atomic_inc_64_nv(&key->zk_salt_count) ==
	    ZIO_CRYPT_MAX_SALT_USAGE);

	rw_exit(&key->zk_salt_lock);

	if (salt_change) {
		ret = zio_crypt_key_change_salt(key);
		if (ret)
			goto error;
	}

	return (0);

error:
	return (ret);
}

/*
 * This function handles all encryption and decryption in zfs. When
 * encrypting it expects puio to refernce the plaintext and cuio to
 * have enough space for the ciphertext + room for a MAC. On decrypting
 * it expects both puio and cuio to have enough room for a MAC, although
 * the plaintext uio can be dsicarded afterwards. datalen should be the
 * length of only the plaintext / ciphertext in either case.
 */
int
zio_do_crypt_uio(boolean_t encrypt, uint64_t crypt, crypto_key_t *key,
    crypto_ctx_template_t tmpl, uint8_t *ivbuf, uint_t datalen,
    uio_t *puio, uio_t *cuio)
{
	int ret;
	crypto_data_t plaindata, cipherdata;
	CK_AES_CCM_PARAMS ccmp;
	CK_AES_GCM_PARAMS gcmp;
	crypto_mechanism_t mech;
	zio_crypt_info_t crypt_info;
	uint_t plain_full_len, maclen;

	ASSERT(crypt < ZIO_CRYPT_FUNCTIONS);
	ASSERT(key->ck_format == CRYPTO_KEY_RAW);

	/* lookup the encryption info */
	crypt_info = zio_crypt_table[crypt];

	/* the mac will always be the last iovec_t in the cipher uio */
	maclen = cuio->uio_iov[cuio->uio_iovcnt - 1].iov_len;

	ASSERT(maclen <= DATA_MAC_LEN);

	/* setup encryption mechanism (same as crypt) */
	mech.cm_type = crypto_mech2id(crypt_info.ci_mechname);

	/* plain length will include the MAC if we are decrypting */
	if (encrypt) {
		plain_full_len = datalen;
	} else {
		plain_full_len = datalen + maclen;
	}

	/*
	 * setup encryption params (currently only AES CCM and AES GCM
	 * are supported)
	 */
	if (crypt_info.ci_crypt_type == ZC_TYPE_CCM) {
		ccmp.ulNonceSize = DATA_IV_LEN;
		ccmp.ulAuthDataSize = 0;
		ccmp.authData = NULL;
		ccmp.ulMACSize = maclen;
		ccmp.nonce = ivbuf;
		ccmp.ulDataSize = plain_full_len;

		mech.cm_param = (char *)(&ccmp);
		mech.cm_param_len = sizeof (CK_AES_CCM_PARAMS);
	} else {
		gcmp.ulIvLen = DATA_IV_LEN;
		gcmp.ulIvBits = BYTES_TO_BITS(DATA_IV_LEN);
		gcmp.ulAADLen = 0;
		gcmp.pAAD = NULL;
		gcmp.ulTagBits = BYTES_TO_BITS(maclen);
		gcmp.pIv = ivbuf;

		mech.cm_param = (char *)(&gcmp);
		mech.cm_param_len = sizeof (CK_AES_GCM_PARAMS);
	}

	/* populate the cipher and plain data structs. */
	plaindata.cd_format = CRYPTO_DATA_UIO;
	plaindata.cd_offset = 0;
	plaindata.cd_uio = puio;
	plaindata.cd_miscdata = NULL;
	plaindata.cd_length = plain_full_len;

	cipherdata.cd_format = CRYPTO_DATA_UIO;
	cipherdata.cd_offset = 0;
	cipherdata.cd_uio = cuio;
	cipherdata.cd_miscdata = NULL;
	cipherdata.cd_length = datalen + maclen;

	/* perform the actual encryption */
	if (encrypt) {
		ret = crypto_encrypt(&mech, &plaindata, key, tmpl, &cipherdata,
		    NULL);
	} else {
		ret = crypto_decrypt(&mech, &cipherdata, key, tmpl, &plaindata,
		    NULL);
	}

	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	return (0);

error:
	return (ret);
}

int
zio_crypt_key_wrap(crypto_key_t *cwkey, zio_crypt_key_t *key, uint8_t *iv,
    uint8_t *mac, uint8_t *keydata_out, uint8_t *hmac_keydata_out)
{
	int ret;
	uio_t puio, cuio;
	iovec_t plain_iovecs[2], cipher_iovecs[3];
	uint64_t crypt = key->zk_crypt;
	uint_t enc_len, keydata_len;

	ASSERT3U(crypt, <, ZIO_CRYPT_FUNCTIONS);
	ASSERT3U(cwkey->ck_format, ==, CRYPTO_KEY_RAW);

	keydata_len = zio_crypt_table[crypt].ci_keylen;

	/* generate iv for wrapping the master and hmac key */
	ret = random_get_pseudo_bytes(iv, WRAPPING_IV_LEN);
	if (ret)
		goto error;

	/* initialize uio_ts */
	plain_iovecs[0].iov_base = key->zk_master_keydata;
	plain_iovecs[0].iov_len = keydata_len;
	plain_iovecs[1].iov_base = key->zk_hmac_keydata;
	plain_iovecs[1].iov_len = HMAC_SHA256_KEYLEN;

	cipher_iovecs[0].iov_base = keydata_out;
	cipher_iovecs[0].iov_len = keydata_len;
	cipher_iovecs[1].iov_base = hmac_keydata_out;
	cipher_iovecs[1].iov_len = HMAC_SHA256_KEYLEN;
	cipher_iovecs[2].iov_base = mac;
	cipher_iovecs[2].iov_len = WRAPPING_MAC_LEN;

	enc_len = zio_crypt_table[crypt].ci_keylen + HMAC_SHA256_KEYLEN;
	puio.uio_iov = plain_iovecs;
	puio.uio_iovcnt = 2;
	puio.uio_segflg = UIO_SYSSPACE;
	cuio.uio_iov = cipher_iovecs;
	cuio.uio_iovcnt = 3;
	cuio.uio_segflg = UIO_SYSSPACE;

	/* encrypt the keys and store the resulting ciphertext and mac */
	ret = zio_encrypt_uio(crypt, cwkey, NULL, iv, enc_len, &puio, &cuio);
	if (ret)
		goto error;

	return (0);

error:
	return (ret);
}

int
zio_crypt_key_unwrap(crypto_key_t *cwkey, uint64_t crypt, uint8_t *keydata,
    uint8_t *hmac_keydata, uint8_t *iv, uint8_t *mac, zio_crypt_key_t *key)
{
	int ret;
	crypto_mechanism_t mech;
	uio_t puio, cuio;
	iovec_t plain_iovecs[3], cipher_iovecs[3];
	uint8_t outmac[WRAPPING_MAC_LEN];
	uint_t enc_len, keydata_len;

	ASSERT3U(crypt, <, ZIO_CRYPT_FUNCTIONS);
	ASSERT3U(cwkey->ck_format, ==, CRYPTO_KEY_RAW);

	keydata_len = zio_crypt_table[crypt].ci_keylen;

	/* initialize uio_ts */
	plain_iovecs[0].iov_base = key->zk_master_keydata;
	plain_iovecs[0].iov_len = keydata_len;
	plain_iovecs[1].iov_base = key->zk_hmac_keydata;
	plain_iovecs[1].iov_len = HMAC_SHA256_KEYLEN;
	plain_iovecs[2].iov_base = outmac;
	plain_iovecs[2].iov_len = WRAPPING_MAC_LEN;

	cipher_iovecs[0].iov_base = keydata;
	cipher_iovecs[0].iov_len = keydata_len;
	cipher_iovecs[1].iov_base = hmac_keydata;
	cipher_iovecs[1].iov_len = HMAC_SHA256_KEYLEN;
	cipher_iovecs[2].iov_base = mac;
	cipher_iovecs[2].iov_len = WRAPPING_MAC_LEN;

	enc_len = keydata_len + HMAC_SHA256_KEYLEN;
	puio.uio_iov = plain_iovecs;
	puio.uio_segflg = UIO_SYSSPACE;
	puio.uio_iovcnt = 3;
	cuio.uio_iov = cipher_iovecs;
	cuio.uio_iovcnt = 3;
	cuio.uio_segflg = UIO_SYSSPACE;

	/* decrypt the keys and store the result in the output buffers */
	ret = zio_decrypt_uio(crypt, cwkey, NULL, iv, enc_len, &puio, &cuio);
	if (ret)
		goto error;

	/* generate a fresh salt */
	ret = random_get_bytes(key->zk_salt, DATA_SALT_LEN);
	if (ret)
		goto error;

	/* derive the current key from the master key */
	ret = hkdf_sha256(key->zk_master_keydata, keydata_len, NULL, 0,
	    key->zk_salt, DATA_SALT_LEN, key->zk_current_keydata, keydata_len);
	if (ret)
		goto error;

	/* initialize keys for ICP */
	key->zk_current_key.ck_format = CRYPTO_KEY_RAW;
	key->zk_current_key.ck_data = key->zk_current_keydata;
	key->zk_current_key.ck_length = BYTES_TO_BITS(keydata_len);

	key->zk_hmac_key.ck_format = CRYPTO_KEY_RAW;
	key->zk_hmac_key.ck_data = key->zk_hmac_keydata;
	key->zk_hmac_key.ck_length = BYTES_TO_BITS(HMAC_SHA256_KEYLEN);

	/*
	 * Initialize the crypto templates. It's ok if this fails because
	 * this is just an optimization.
	 */
	mech.cm_type = crypto_mech2id(zio_crypt_table[crypt].ci_mechname);
	ret = crypto_create_ctx_template(&mech, &key->zk_current_key,
	    &key->zk_current_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->zk_current_tmpl = NULL;

	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	ret = crypto_create_ctx_template(&mech, &key->zk_hmac_key,
	    &key->zk_hmac_tmpl, KM_SLEEP);
	if (ret != CRYPTO_SUCCESS)
		key->zk_hmac_tmpl = NULL;

	key->zk_crypt = crypt;
	key->zk_salt_count = 0;
	rw_init(&key->zk_salt_lock, NULL, RW_DEFAULT, NULL);

	return (0);

error:
	zio_crypt_key_destroy(key);
	return (ret);
}

int
zio_crypt_generate_iv(blkptr_t *bp, dmu_object_type_t ot, uint8_t *salt,
    uint64_t txgid, zbookmark_phys_t *zb, uint8_t *ivbuf)
{
	int ret;
	crypto_mechanism_t mech;
	crypto_context_t ctx;
	crypto_data_t in_data, digest_data;
	uint8_t digestbuf[SHA_256_DIGEST_LEN];

	/* initialize sha 256 mechanism and crypto data */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	in_data.cd_format = CRYPTO_DATA_RAW;
	in_data.cd_offset = 0;

	digest_data.cd_format = CRYPTO_DATA_RAW;
	digest_data.cd_offset = 0;
	digest_data.cd_length = SHA_256_DIGEST_LEN;
	digest_data.cd_raw.iov_base = (char *)digestbuf;
	digest_data.cd_raw.iov_len = SHA_256_DIGEST_LEN;

	/* initialize the context */
	ret = crypto_digest_init(&mech, &ctx, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in DVA[0] */
	in_data.cd_length = sizeof (dva_t);
	in_data.cd_raw.iov_base = (char *)BP_IDENTITY(bp);
	in_data.cd_raw.iov_len = sizeof (dva_t);

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/*
	 * For most object types we use the txgid here. DVA[0] + txgid should
	 * give us a unique value for the IV. However, ZIL blocks will have a
	 * birth txg of 0 on write since they are preallocated, but they will
	 * have a real value on replay. In this case, we replace the txgid with
	 * a hash of the zb, which should be unique due to the sequence id.
	 */
	if (ot != DMU_OT_INTENT_LOG) {
		in_data.cd_length = sizeof (uint64_t);
		in_data.cd_raw.iov_base = (char *)&txgid;
		in_data.cd_raw.iov_len = sizeof (uint64_t);
	} else {
		in_data.cd_length = sizeof (zbookmark_phys_t);
		in_data.cd_raw.iov_base = (char *)zb;
		in_data.cd_raw.iov_len = sizeof (zbookmark_phys_t);
	}

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in the salt for added protection against pool rewinds */
	in_data.cd_length = DATA_SALT_LEN;
	in_data.cd_raw.iov_base = (char *)salt;
	in_data.cd_raw.iov_len = DATA_SALT_LEN;

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* finish the hash */
	ret = crypto_digest_final(ctx, &digest_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* truncate and copy the digest into the output buffer */
	bcopy(digestbuf, ivbuf, DATA_IV_LEN);

	return (0);

error:
	return (ret);
}

int
zio_crypt_generate_iv_salt_dedup(zio_crypt_key_t *key, uint8_t *data,
    uint_t datalen, uint8_t *ivbuf, uint8_t *salt)
{
	int ret;
	crypto_mechanism_t mech;
	crypto_data_t in_data, digest_data;
	uint8_t digestbuf[SHA_256_DIGEST_LEN];

	/* initialize sha256-hmac mechanism and crypto data */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256_HMAC);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	/* initialize the crypto data */
	in_data.cd_format = CRYPTO_DATA_RAW;
	in_data.cd_offset = 0;
	in_data.cd_length = datalen;
	in_data.cd_raw.iov_base = (char *)data;
	in_data.cd_raw.iov_len = datalen;

	digest_data.cd_format = CRYPTO_DATA_RAW;
	digest_data.cd_offset = 0;
	digest_data.cd_length = SHA_256_DIGEST_LEN;
	digest_data.cd_raw.iov_base = (char *)digestbuf;
	digest_data.cd_raw.iov_len = SHA_256_DIGEST_LEN;

	/* generate the hmac */
	ret = crypto_mac(&mech, &in_data, &key->zk_hmac_key, key->zk_hmac_tmpl,
	    &digest_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* truncate and copy the digest into the output buffer */
	bcopy(digestbuf, salt, DATA_SALT_LEN);
	bcopy(digestbuf + DATA_SALT_LEN, ivbuf, DATA_IV_LEN);

	return (0);

error:
	return (ret);
}

int
zio_crypt_generate_iv_l2arc(uint64_t spa, dva_t *dva, uint64_t birth,
    uint64_t daddr, uint8_t *ivbuf)
{
	int ret;
	crypto_mechanism_t mech;
	crypto_context_t ctx;
	crypto_data_t in_data, digest_data;
	uint8_t digestbuf[SHA_256_DIGEST_LEN];

	/* initialize sha256 mechanism and crypto data */
	mech.cm_type = crypto_mech2id(SUN_CKM_SHA256);
	mech.cm_param = NULL;
	mech.cm_param_len = 0;

	in_data.cd_format = CRYPTO_DATA_RAW;
	in_data.cd_offset = 0;

	digest_data.cd_format = CRYPTO_DATA_RAW;
	digest_data.cd_offset = 0;
	digest_data.cd_length = SHA_256_DIGEST_LEN;
	digest_data.cd_raw.iov_base = (char *)digestbuf;
	digest_data.cd_raw.iov_len = SHA_256_DIGEST_LEN;

	/* initialize the context */
	ret = crypto_digest_init(&mech, &ctx, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in the spa */
	in_data.cd_length = sizeof (uint64_t);
	in_data.cd_raw.iov_base = (char *)&spa;
	in_data.cd_raw.iov_len = sizeof (uint64_t);

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in the dva */
	in_data.cd_length = sizeof (dva_t);
	in_data.cd_raw.iov_base = (char *)dva;
	in_data.cd_raw.iov_len = sizeof (dva_t);

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in the birth */
	in_data.cd_length = sizeof (uint64_t);
	in_data.cd_raw.iov_base = (char *)&birth;
	in_data.cd_raw.iov_len = sizeof (uint64_t);

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* add in the daddr */
	in_data.cd_length = sizeof (uint64_t);
	in_data.cd_raw.iov_base = (char *)&daddr;
	in_data.cd_raw.iov_len = sizeof (uint64_t);

	ret = crypto_digest_update(ctx, &in_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* finish the hash */
	ret = crypto_digest_final(ctx, &digest_data, NULL);
	if (ret != CRYPTO_SUCCESS) {
		ret = SET_ERROR(EIO);
		goto error;
	}

	/* truncate and copy the digest into the output buffer */
	bcopy(digestbuf, ivbuf, L2ARC_IV_LEN);

	return (0);

error:
	return (ret);
}

static void zio_crypt_destroy_uio(uio_t *uio) {
	if (uio->uio_iov)
		kmem_free(uio->uio_iov, uio->uio_iovcnt * sizeof (iovec_t));
}

/*
 * We do not check for the older zil chain because this feature was not
 * available before the newer zil chain was introduced. The goal here
 * is to encrypt everything except the blkptr_t of a lr_write_t and
 * the zil_chain_t header.
 */
static int
zio_crypt_init_uios_zil(boolean_t encrypt, uint8_t *plainbuf,
    uint8_t *cipherbuf, uint_t datalen, uio_t *puio, uio_t *cuio,
    uint_t *enc_len)
{
	int ret;
	uint_t nr_src, nr_dst, lr_len, crypt_len, nr_iovecs = 0, total_len = 0;
	iovec_t *src_iovecs = NULL, *dst_iovecs = NULL;
	uint8_t *src, *dst, *slrp, *dlrp, *end;
	zil_chain_t *zilc;
	lr_t *lr;

	/* if we are decrypting, the plainbuffer needs an extra iovec */
	if (encrypt) {
		src = plainbuf;
		dst = cipherbuf;
		nr_src = 0;
		nr_dst = 1;
	} else {
		src = cipherbuf;
		dst = plainbuf;
		nr_src = 1;
		nr_dst = 1;
	}

	/* find the start and end record of the log block */
	zilc = (zil_chain_t *) src;
	end = src + zilc->zc_nused;
	slrp = src + sizeof (zil_chain_t);

	/* calculate the number of encrypted iovecs we will need */
	for (; slrp < end; slrp += lr_len) {
		lr = (lr_t *) slrp;
		lr_len = lr->lrc_reclen;

		nr_iovecs++;
		if (lr->lrc_txtype == TX_WRITE &&
		    lr_len != sizeof (lr_write_t))
			nr_iovecs++;
	}

	if (nr_iovecs == 0) {
		puio->uio_iov = NULL;
		puio->uio_iovcnt = 0;
		cuio->uio_iov = NULL;
		cuio->uio_iovcnt = 0;
		return (ZIO_NO_ENCRYPTION_NEEDED);
	}

	nr_src += nr_iovecs;
	nr_dst += nr_iovecs;

	/* allocate the iovec arrays */
	src_iovecs = kmem_alloc(nr_src * sizeof (iovec_t), KM_SLEEP);
	if (!src_iovecs) {
		ret = SET_ERROR(ENOMEM);
		goto error;
	}

	dst_iovecs = kmem_alloc(nr_dst * sizeof (iovec_t), KM_SLEEP);
	if (!dst_iovecs) {
		ret = SET_ERROR(ENOMEM);
		goto error;
	}

	/* loop over records again, filling in iovecs */
	nr_iovecs = 0;
	slrp = src + sizeof (zil_chain_t);
	dlrp = dst + sizeof (zil_chain_t);

	for (; slrp < end; slrp += lr_len, dlrp += lr_len) {
		lr = (lr_t *) slrp;
		lr_len = lr->lrc_reclen;

		if (lr->lrc_txtype == TX_WRITE) {
			bcopy(slrp, dlrp, sizeof (lr_t));
			crypt_len = sizeof (lr_write_t) -
			    sizeof (lr_t) - sizeof (blkptr_t);
			src_iovecs[nr_iovecs].iov_base = slrp + sizeof (lr_t);
			src_iovecs[nr_iovecs].iov_len = crypt_len;
			dst_iovecs[nr_iovecs].iov_base = dlrp + sizeof (lr_t);
			dst_iovecs[nr_iovecs].iov_len = crypt_len;

			/* copy the bp now since it will not be encrypted */
			bcopy(slrp + sizeof (lr_write_t) - sizeof (blkptr_t),
			    dlrp + sizeof (lr_write_t) - sizeof (blkptr_t),
			    sizeof (blkptr_t));
			nr_iovecs++;
			total_len += crypt_len;

			if (lr_len != sizeof (lr_write_t)) {
				crypt_len = lr_len - sizeof (lr_write_t);
				src_iovecs[nr_iovecs].iov_base =
				    slrp + sizeof (lr_write_t);
				src_iovecs[nr_iovecs].iov_len = crypt_len;
				dst_iovecs[nr_iovecs].iov_base =
				    dlrp + sizeof (lr_write_t);
				dst_iovecs[nr_iovecs].iov_len = crypt_len;
				nr_iovecs++;
				total_len += crypt_len;
			}
		} else {
			bcopy(slrp, dlrp, sizeof (lr_t));
			crypt_len = lr_len - sizeof (lr_t);
			src_iovecs[nr_iovecs].iov_base = slrp + sizeof (lr_t);
			src_iovecs[nr_iovecs].iov_len = crypt_len;
			dst_iovecs[nr_iovecs].iov_base = dlrp + sizeof (lr_t);
			dst_iovecs[nr_iovecs].iov_len = crypt_len;
			nr_iovecs++;
			total_len += crypt_len;
		}
	}

	/* copy the plain zil header over */
	bcopy(src, dst, sizeof (zil_chain_t));

	*enc_len = total_len;

	if (encrypt) {
		puio->uio_iov = src_iovecs;
		puio->uio_iovcnt = nr_src;
		cuio->uio_iov = dst_iovecs;
		cuio->uio_iovcnt = nr_dst;
	} else {
		puio->uio_iov = dst_iovecs;
		puio->uio_iovcnt = nr_dst;
		cuio->uio_iov = src_iovecs;
		cuio->uio_iovcnt = nr_src;
	}

	return (0);

error:
	if (src_iovecs)
		kmem_free(src_iovecs, nr_src * sizeof (iovec_t));
	if (dst_iovecs)
		kmem_free(dst_iovecs, nr_dst * sizeof (iovec_t));

	*enc_len = 0;
	puio->uio_iov = NULL;
	puio->uio_iovcnt = 0;
	cuio->uio_iov = NULL;
	cuio->uio_iovcnt = 0;
	return (ret);
}

static int
zio_crypt_init_uios_normal(boolean_t encrypt, uint8_t *plainbuf,
    uint8_t *cipherbuf, uint_t datalen, uio_t *puio, uio_t *cuio,
    uint_t *enc_len)
{
	int ret;
	uint_t nr_plain, nr_cipher;
	iovec_t *plain_iovecs = NULL, *cipher_iovecs = NULL;

	/* allocate the iovecs for the plain and cipher data */
	if (encrypt) {
		nr_plain = 1;
		plain_iovecs = kmem_alloc(nr_plain * sizeof (iovec_t),
		    KM_SLEEP);
		if (!plain_iovecs) {
			ret = SET_ERROR(ENOMEM);
			goto error;
		}

		nr_cipher = 2;
		cipher_iovecs = kmem_alloc(nr_cipher * sizeof (iovec_t),
		    KM_SLEEP);
		if (!cipher_iovecs) {
			ret = SET_ERROR(ENOMEM);
			goto error;
		}
	} else {
		nr_plain = 2;
		plain_iovecs = kmem_alloc(nr_plain * sizeof (iovec_t),
		    KM_SLEEP);
		if (!plain_iovecs) {
			ret = SET_ERROR(ENOMEM);
			goto error;
		}

		nr_cipher = 2;
		cipher_iovecs = kmem_alloc(nr_cipher * sizeof (iovec_t),
		    KM_SLEEP);
		if (!cipher_iovecs) {
			ret = SET_ERROR(ENOMEM);
			goto error;
		}
	}

	plain_iovecs[0].iov_base = plainbuf;
	plain_iovecs[0].iov_len = datalen;
	cipher_iovecs[0].iov_base = cipherbuf;
	cipher_iovecs[0].iov_len = datalen;

	*enc_len = datalen;
	puio->uio_iov = plain_iovecs;
	puio->uio_iovcnt = nr_plain;
	cuio->uio_iov = cipher_iovecs;
	cuio->uio_iovcnt = nr_cipher;

	return (0);

error:
	if (plain_iovecs)
		kmem_free(plain_iovecs, nr_plain * sizeof (iovec_t));
	if (cipher_iovecs)
		kmem_free(cipher_iovecs, nr_cipher * sizeof (iovec_t));

	*enc_len = 0;
	puio->uio_iov = NULL;
	puio->uio_iovcnt = 0;
	cuio->uio_iov = NULL;
	cuio->uio_iovcnt = 0;
	return (ret);
}

static int
zio_crypt_init_uios(boolean_t encrypt, dmu_object_type_t ot, uint8_t *plainbuf,
    uint8_t *cipherbuf, uint_t datalen, uint8_t *mac, uint8_t *out_mac,
    uio_t *puio, uio_t *cuio, uint_t *enc_len)
{
	int ret;
	uint_t maclen;
	iovec_t *mac_iov, *mac_out_iov;

	ASSERT(DMU_OT_IS_ENCRYPTED(ot));

	/* route to handler */
	if (ot == DMU_OT_INTENT_LOG) {
		ret = zio_crypt_init_uios_zil(encrypt, plainbuf, cipherbuf,
		    datalen, puio, cuio, enc_len);
		maclen = ZIL_MAC_LEN;
	} else {
		ret = zio_crypt_init_uios_normal(encrypt, plainbuf, cipherbuf,
		    datalen, puio, cuio, enc_len);
		maclen = DATA_MAC_LEN;
	}

	if (ret == ZIO_NO_ENCRYPTION_NEEDED) {
		bzero(mac, maclen);
		return (ret);
	} else if (ret) {
		goto error;
	}

	/* populate the uios */
	puio->uio_segflg = UIO_SYSSPACE;
	cuio->uio_segflg = UIO_SYSSPACE;

	mac_iov = ((iovec_t *)&cuio->uio_iov[cuio->uio_iovcnt - 1]);
	mac_iov->iov_base = mac;
	mac_iov->iov_len = maclen;

	if (!encrypt) {
		mac_out_iov = ((iovec_t *)&puio->uio_iov[puio->uio_iovcnt - 1]);
		mac_out_iov->iov_base = out_mac;
		mac_out_iov->iov_len = maclen;
	}

	return (0);

error:
	return (ret);
}

int
zio_do_crypt_data(boolean_t encrypt, zio_crypt_key_t *key, uint8_t *salt,
    dmu_object_type_t ot, uint8_t *iv, uint8_t *mac, uint_t datalen,
    uint8_t *plainbuf, uint8_t *cipherbuf)
{
	int ret;
	boolean_t locked = B_FALSE;
	uint64_t crypt = key->zk_crypt;
	uint_t enc_len, keydata_len = zio_crypt_table[crypt].ci_keylen;
	uio_t puio, cuio;
	uint8_t out_mac[DATA_MAC_LEN];
	uint8_t enc_keydata[MAX_MASTER_KEY_LEN];
	crypto_key_t tmp_ckey, *ckey = NULL;
	crypto_ctx_template_t tmpl;

	bzero(&puio, sizeof (uio_t));
	bzero(&cuio, sizeof (uio_t));

	/* create uios for encryption */
	ret = zio_crypt_init_uios(encrypt, ot, plainbuf, cipherbuf, datalen,
	    mac, out_mac, &puio, &cuio, &enc_len);

	/* if no crypto work is required, just copy the plain data */
	if (ret == ZIO_NO_ENCRYPTION_NEEDED) {
		if (encrypt) {
			bcopy(plainbuf, cipherbuf, datalen);
		} else {
			bcopy(cipherbuf, plainbuf, datalen);
		}
		return (0);
	} else if (ret) {
		return (ret);
	}

	/*
	 * If the needed key is the current one, just use it. Otherwise we
	 * need to generate a temporary one from the given salt + master key.
	 * If we are encrypting, we must return a copy of the current salt
	 * so that it can be stored in the blkptr_t.
	 */
	rw_enter(&key->zk_salt_lock, RW_READER);
	locked = B_TRUE;

	if (bcmp(salt, key->zk_salt, DATA_SALT_LEN) == 0) {
		ckey = &key->zk_current_key;
		tmpl = key->zk_current_tmpl;
	} else {
		rw_exit(&key->zk_salt_lock);
		locked = B_FALSE;

		ret = hkdf_sha256(key->zk_master_keydata, keydata_len, NULL, 0,
		    salt, DATA_SALT_LEN, enc_keydata, keydata_len);
		if (ret)
			goto error;

		tmp_ckey.ck_format = CRYPTO_KEY_RAW;
		tmp_ckey.ck_data = enc_keydata;
		tmp_ckey.ck_length = BYTES_TO_BITS(keydata_len);

		ckey = &tmp_ckey;
		tmpl = NULL;
	}

	/* perform the encryption / decryption */
	ret = zio_do_crypt_uio(encrypt, key->zk_crypt, ckey, tmpl, iv, enc_len,
	    &puio, &cuio);
	if (ret)
		goto error;

	if (locked) {
		rw_exit(&key->zk_salt_lock);
		locked = B_FALSE;
	}

	if (ckey == &tmp_ckey)
		bzero(enc_keydata, keydata_len);
	zio_crypt_destroy_uio(&puio);
	zio_crypt_destroy_uio(&cuio);

	return (0);

error:
	if (locked)
		rw_exit(&key->zk_salt_lock);
	if (ckey == &tmp_ckey)
		bzero(enc_keydata, keydata_len);
	zio_crypt_destroy_uio(&puio);
	zio_crypt_destroy_uio(&cuio);

	return (ret);
}
