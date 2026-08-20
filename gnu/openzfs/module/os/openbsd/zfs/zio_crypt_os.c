// SPDX-License-Identifier: CDDL-1.0
/*
 * OpenBSD deliberately does not provide ZFS native encryption.  Keep the
 * on-disk interfaces linkable so unencrypted pools can use the common ZFS
 * code, but fail every operation which would handle encrypted data.
 */

#include <sys/zfs_context.h>
#include <sys/zio_crypt.h>

const zio_crypt_info_t zio_crypt_table[ZIO_CRYPT_FUNCTIONS];

void
zio_crypt_key_destroy(zio_crypt_key_t *key)
{

	memset(key, 0, sizeof (*key));
}

int
zio_crypt_key_init(uint64_t crypt, zio_crypt_key_t *key)
{

	(void)crypt;
	memset(key, 0, sizeof (*key));
	return (EOPNOTSUPP);
}

int
zio_crypt_key_get_salt(zio_crypt_key_t *key, uint8_t *salt_out)
{

	(void)key;
	memset(salt_out, 0, ZIO_DATA_SALT_LEN);
	return (EOPNOTSUPP);
}

int
zio_crypt_key_wrap(crypto_key_t *cwkey, zio_crypt_key_t *key, uint8_t *iv,
    uint8_t *mac, uint8_t *keydata_out, uint8_t *hmac_keydata_out)
{

	(void)cwkey;
	(void)key;
	(void)iv;
	(void)mac;
	(void)keydata_out;
	(void)hmac_keydata_out;
	return (EOPNOTSUPP);
}

int
zio_crypt_key_unwrap(crypto_key_t *cwkey, uint64_t crypt, uint64_t version,
    uint64_t guid, uint8_t *keydata, uint8_t *hmac_keydata, uint8_t *iv,
    uint8_t *mac, zio_crypt_key_t *key)
{

	(void)cwkey;
	(void)crypt;
	(void)version;
	(void)guid;
	(void)keydata;
	(void)hmac_keydata;
	(void)iv;
	(void)mac;
	memset(key, 0, sizeof (*key));
	return (EOPNOTSUPP);
}

int
zio_crypt_generate_iv(uint8_t *ivbuf)
{

	memset(ivbuf, 0, ZIO_DATA_IV_LEN);
	return (EOPNOTSUPP);
}

int
zio_crypt_generate_iv_salt_dedup(zio_crypt_key_t *key, uint8_t *data,
    uint_t datalen, uint8_t *ivbuf, uint8_t *salt)
{

	(void)key;
	(void)data;
	(void)datalen;
	memset(ivbuf, 0, ZIO_DATA_IV_LEN);
	memset(salt, 0, ZIO_DATA_SALT_LEN);
	return (EOPNOTSUPP);
}

void
zio_crypt_encode_params_bp(blkptr_t *bp, uint8_t *salt, uint8_t *iv)
{

	(void)bp;
	(void)salt;
	(void)iv;
}

void
zio_crypt_decode_params_bp(const blkptr_t *bp, uint8_t *salt, uint8_t *iv)
{

	(void)bp;
	memset(salt, 0, ZIO_DATA_SALT_LEN);
	memset(iv, 0, ZIO_DATA_IV_LEN);
}

void
zio_crypt_encode_mac_bp(blkptr_t *bp, uint8_t *mac)
{

	(void)bp;
	(void)mac;
}

void
zio_crypt_decode_mac_bp(const blkptr_t *bp, uint8_t *mac)
{

	(void)bp;
	memset(mac, 0, ZIO_DATA_MAC_LEN);
}

void
zio_crypt_encode_mac_zil(void *data, uint8_t *mac)
{

	(void)data;
	(void)mac;
}

void
zio_crypt_decode_mac_zil(const void *data, uint8_t *mac)
{

	(void)data;
	memset(mac, 0, ZIO_DATA_MAC_LEN);
}

void
zio_crypt_copy_dnode_bonus(abd_t *src_abd, uint8_t *dst, uint_t datalen)
{

	(void)src_abd;
	memset(dst, 0, datalen);
}

int
zio_crypt_do_indirect_mac_checksum(boolean_t generate, void *buf,
    uint_t datalen, boolean_t byteswap, uint8_t *cksum)
{

	(void)generate;
	(void)buf;
	(void)datalen;
	(void)byteswap;
	memset(cksum, 0, ZIO_DATA_MAC_LEN);
	return (EOPNOTSUPP);
}

int
zio_crypt_do_indirect_mac_checksum_abd(boolean_t generate, abd_t *abd,
    uint_t datalen, boolean_t byteswap, uint8_t *cksum)
{

	(void)generate;
	(void)abd;
	(void)datalen;
	(void)byteswap;
	memset(cksum, 0, ZIO_DATA_MAC_LEN);
	return (EOPNOTSUPP);
}

int
zio_crypt_do_hmac(zio_crypt_key_t *key, uint8_t *data, uint_t datalen,
    uint8_t *digestbuf, uint_t digestlen)
{

	(void)key;
	(void)data;
	(void)datalen;
	memset(digestbuf, 0, digestlen);
	return (EOPNOTSUPP);
}

int
zio_crypt_do_objset_hmacs(zio_crypt_key_t *key, void *data, uint_t datalen,
    boolean_t byteswap, uint8_t *portable_mac, uint8_t *local_mac)
{

	(void)key;
	(void)data;
	(void)datalen;
	(void)byteswap;
	memset(portable_mac, 0, ZIO_OBJSET_MAC_LEN);
	memset(local_mac, 0, ZIO_OBJSET_MAC_LEN);
	return (EOPNOTSUPP);
}

int
zio_do_crypt_data(boolean_t encrypt, zio_crypt_key_t *key,
    dmu_object_type_t ot, boolean_t byteswap, uint8_t *salt, uint8_t *iv,
    uint8_t *mac, uint_t datalen, uint8_t *plainbuf, uint8_t *cipherbuf,
    boolean_t *no_crypt)
{

	(void)encrypt;
	(void)key;
	(void)ot;
	(void)byteswap;
	(void)salt;
	(void)iv;
	(void)mac;
	(void)datalen;
	(void)plainbuf;
	(void)cipherbuf;
	if (no_crypt != NULL)
		*no_crypt = B_FALSE;
	return (EOPNOTSUPP);
}

int
zio_do_crypt_abd(boolean_t encrypt, zio_crypt_key_t *key,
    dmu_object_type_t ot, boolean_t byteswap, uint8_t *salt, uint8_t *iv,
    uint8_t *mac, uint_t datalen, abd_t *pabd, abd_t *cabd,
    boolean_t *no_crypt)
{

	(void)encrypt;
	(void)key;
	(void)ot;
	(void)byteswap;
	(void)salt;
	(void)iv;
	(void)mac;
	(void)datalen;
	(void)pabd;
	(void)cabd;
	if (no_crypt != NULL)
		*no_crypt = B_FALSE;
	return (EOPNOTSUPP);
}
