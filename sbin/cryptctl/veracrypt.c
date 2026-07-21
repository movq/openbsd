/*	$OpenBSD$	*/

/*
 * Copyright (c) 2026 Mike Jones <mike@mjones.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/endian.h>

#include <errno.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "veracrypt.h"

#define VC_HEADER_SIZE			512
#define VC_HEADER_GROUP_SIZE		(128 * 1024)
#define VC_TOTAL_HEADER_SIZE		(2 * VC_HEADER_GROUP_SIZE)
#define VC_SALT_SIZE			64
#define VC_ENCRYPTED_HEADER_SIZE	(VC_HEADER_SIZE - VC_SALT_SIZE)
#define VC_KEY_AREA_OFFSET		256
#define VC_KEY_AREA_SIZE		256
#define VC_DATA_OFFSET			VC_HEADER_GROUP_SIZE
#define VC_DATA_UNIT_SIZE		512
#define VC_SECTOR_SIZE_MAX		4096
#define VC_HEADER_VERSION_MIN		4
#define VC_HEADER_VERSION_MAX		5
#define VC_REQUIRED_VERSION_MAX		0x010b
#define VC_HEADER_FLAG_SYSTEM		0x00000001
#define VC_HEADER_FLAG_INPLACE		0x00000002

#define VC_OFF_MAGIC			64
#define VC_OFF_VERSION			68
#define VC_OFF_REQUIRED_VERSION		70
#define VC_OFF_KEY_CRC			72
#define VC_OFF_HIDDEN_SIZE		92
#define VC_OFF_VOLUME_SIZE		100
#define VC_OFF_DATA_OFFSET		108
#define VC_OFF_DATA_LENGTH		116
#define VC_OFF_FLAGS			124
#define VC_OFF_SECTOR_SIZE		128
#define VC_OFF_HEADER_CRC		252

static uint16_t	get_be16(const uint8_t *);
static uint32_t	get_be32(const uint8_t *);
static uint64_t	get_be64(const uint8_t *);
static uint32_t	vc_crc32(const uint8_t *, size_t);
static int	vc_decrypt_header(const uint8_t[VC_HEADER_SIZE],
		    const char *, size_t, uint32_t, uint8_t[VC_HEADER_SIZE]);
static int	vc_parse_header(const uint8_t[VC_HEADER_SIZE], uint64_t,
		    struct vc_mapping *, uint8_t[VC_VOLUME_KEY_SIZE],
		    char *, size_t);
static int	vc_read_header(int, uint64_t, const char *, size_t, uint32_t,
		    uint64_t, struct vc_mapping *, uint8_t[VC_VOLUME_KEY_SIZE],
		    char *, size_t);
static int	pread_full(int, void *, size_t, uint64_t);

static uint16_t
get_be16(const uint8_t *p)
{
	uint16_t value;

	memcpy(&value, p, sizeof(value));
	return (betoh16(value));
}

static uint32_t
get_be32(const uint8_t *p)
{
	uint32_t value;

	memcpy(&value, p, sizeof(value));
	return (betoh32(value));
}

static uint64_t
get_be64(const uint8_t *p)
{
	uint64_t value;

	memcpy(&value, p, sizeof(value));
	return (betoh64(value));
}

static uint32_t
vc_crc32(const uint8_t *buf, size_t len)
{
	uint32_t byte, crc = 0xffffffff, mask;
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		byte = buf[i];
		crc ^= byte;
		for (bit = 0; bit < 8; bit++) {
			mask = -(crc & 1);
			crc = (crc >> 1) ^ (0xedb88320 & mask);
		}
	}
	return (~crc);
}

static int
pread_full(int fd, void *buf, size_t len, uint64_t offset)
{
	uint8_t *p = buf;
	ssize_t n;
	size_t done = 0;

	if (offset > INT64_MAX || len > (uint64_t)INT64_MAX - offset) {
		errno = EOVERFLOW;
		return (-1);
	}
	while (done < len) {
		n = pread(fd, p + done, len - done, offset + done);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (n == 0) {
			errno = EIO;
			return (-1);
		}
		done += n;
	}
	return (0);
}

static int
vc_decrypt_header(const uint8_t disk[VC_HEADER_SIZE], const char *passphrase,
    size_t passphrase_len, uint32_t iterations, uint8_t plain[VC_HEADER_SIZE])
{
	EVP_CIPHER_CTX *ctx = NULL;
	uint8_t header_key[VC_VOLUME_KEY_SIZE], iv[16];
	int error = -1;

	memset(header_key, 0, sizeof(header_key));
	memset(iv, 0, sizeof(iv));
	if (passphrase_len > INT_MAX ||
	    PKCS5_PBKDF2_HMAC(passphrase, (int)passphrase_len, disk,
	    VC_SALT_SIZE, iterations, EVP_sha512(), sizeof(header_key),
	    header_key) != 1)
		goto out;
	if ((ctx = EVP_CIPHER_CTX_new()) == NULL ||
	    EVP_DecryptInit_ex(ctx, EVP_aes_256_xts(), NULL, header_key,
	    iv) != 1 ||
	    EVP_Cipher(ctx, plain + VC_SALT_SIZE, plain + VC_SALT_SIZE,
	    VC_ENCRYPTED_HEADER_SIZE) != 1)
		goto out;
	error = 0;

out:
	EVP_CIPHER_CTX_free(ctx);
	explicit_bzero(header_key, sizeof(header_key));
	explicit_bzero(iv, sizeof(iv));
	return (error);
}

static int
vc_parse_header(const uint8_t plain[VC_HEADER_SIZE], uint64_t device_size,
    struct vc_mapping *mapping, uint8_t volume_key[VC_VOLUME_KEY_SIZE],
    char *why, size_t why_size)
{
	uint64_t data_length, data_offset, hidden_size, volume_size;
	uint32_t flags, sector_size;
	uint16_t required_version, version;

	if (memcmp(plain + VC_OFF_MAGIC, "VERA", 4) != 0)
		return (1);
	version = get_be16(plain + VC_OFF_VERSION);
	required_version = get_be16(plain + VC_OFF_REQUIRED_VERSION);
	if (version < VC_HEADER_VERSION_MIN ||
	    version > VC_HEADER_VERSION_MAX ||
	    required_version > VC_REQUIRED_VERSION_MAX) {
		snprintf(why, why_size, "unsupported VeraCrypt header version");
		return (-1);
	}
	if (get_be32(plain + VC_OFF_HEADER_CRC) !=
	    vc_crc32(plain + VC_OFF_MAGIC,
	    VC_OFF_HEADER_CRC - VC_OFF_MAGIC) ||
	    get_be32(plain + VC_OFF_KEY_CRC) !=
	    vc_crc32(plain + VC_KEY_AREA_OFFSET, VC_KEY_AREA_SIZE)) {
		snprintf(why, why_size, "invalid VeraCrypt header checksum");
		return (-1);
	}

	hidden_size = get_be64(plain + VC_OFF_HIDDEN_SIZE);
	volume_size = get_be64(plain + VC_OFF_VOLUME_SIZE);
	data_offset = get_be64(plain + VC_OFF_DATA_OFFSET);
	data_length = get_be64(plain + VC_OFF_DATA_LENGTH);
	flags = get_be32(plain + VC_OFF_FLAGS);
	sector_size = version < 5 ? VC_DATA_UNIT_SIZE :
	    get_be32(plain + VC_OFF_SECTOR_SIZE);

	if (hidden_size != 0 || (flags &
	    (VC_HEADER_FLAG_SYSTEM | VC_HEADER_FLAG_INPLACE)) != 0) {
		snprintf(why, why_size,
		    "hidden, system, and in-place VeraCrypt volumes "
		    "are not supported");
		return (-1);
	}
	if ((flags & ~(VC_HEADER_FLAG_SYSTEM | VC_HEADER_FLAG_INPLACE)) != 0) {
		snprintf(why, why_size, "unsupported VeraCrypt header flags");
		return (-1);
	}
	if (device_size <= VC_TOTAL_HEADER_SIZE ||
	    volume_size != device_size - VC_TOTAL_HEADER_SIZE ||
	    data_offset != VC_DATA_OFFSET ||
	    data_length != volume_size ||
	    data_offset > device_size ||
	    data_length > device_size - data_offset) {
		snprintf(why, why_size, "invalid VeraCrypt volume geometry");
		return (-1);
	}
	if (sector_size < VC_DATA_UNIT_SIZE ||
	    sector_size > VC_SECTOR_SIZE_MAX ||
	    (sector_size & (sector_size - 1)) != 0 ||
	    (data_offset & (sector_size - 1)) != 0 ||
	    (data_length & (sector_size - 1)) != 0) {
		snprintf(why, why_size, "unsupported VeraCrypt sector size");
		return (-1);
	}
	if (timingsafe_bcmp(plain + VC_KEY_AREA_OFFSET,
	    plain + VC_KEY_AREA_OFFSET + VC_VOLUME_KEY_SIZE / 2,
	    VC_VOLUME_KEY_SIZE / 2) == 0) {
		snprintf(why, why_size, "invalid VeraCrypt XTS key");
		return (-1);
	}

	mapping->data_offset = data_offset;
	mapping->data_length = data_length;
	mapping->iv_offset = data_offset / VC_DATA_UNIT_SIZE;
	mapping->sector_size = sector_size;
	mapping->xts_sector_size = VC_DATA_UNIT_SIZE;
	memcpy(volume_key, plain + VC_KEY_AREA_OFFSET, VC_VOLUME_KEY_SIZE);
	return (0);
}

static int
vc_read_header(int fd, uint64_t offset, const char *passphrase,
    size_t passphrase_len, uint32_t iterations, uint64_t device_size,
    struct vc_mapping *mapping, uint8_t volume_key[VC_VOLUME_KEY_SIZE],
    char *why, size_t why_size)
{
	uint8_t disk[VC_HEADER_SIZE], plain[VC_HEADER_SIZE];
	int error = -1;

	if (pread_full(fd, disk, sizeof(disk), offset) == -1) {
		snprintf(why, why_size, "could not read VeraCrypt header: %s",
		    strerror(errno));
		goto out;
	}
	memcpy(plain, disk, sizeof(plain));
	if (vc_decrypt_header(disk, passphrase, passphrase_len, iterations,
	    plain) == -1) {
		snprintf(why, why_size, "could not decrypt VeraCrypt header");
		goto out;
	}
	error = vc_parse_header(plain, device_size, mapping, volume_key,
	    why, why_size);

out:
	explicit_bzero(disk, sizeof(disk));
	explicit_bzero(plain, sizeof(plain));
	return (error);
}

int
vc_read_unlock(int fd, uint64_t device_size, const char *passphrase,
    size_t passphrase_len, uint32_t pim, struct vc_mapping *mapping,
    uint8_t volume_key[VC_VOLUME_KEY_SIZE], char *why, size_t why_size)
{
	uint64_t backup_offset;
	uint32_t iterations;
	int error;

	if (pim > VC_PIM_MAX) {
		snprintf(why, why_size, "invalid VeraCrypt PIM");
		return (-1);
	}
	if (device_size <= VC_TOTAL_HEADER_SIZE) {
		snprintf(why, why_size, "VeraCrypt volume is too small");
		return (-1);
	}
	iterations = pim == 0 ? 500000 : 15000 + pim * 1000;

	error = vc_read_header(fd, 0, passphrase, passphrase_len, iterations,
	    device_size, mapping, volume_key, why, why_size);
	if (error == 0)
		return (0);

	backup_offset = device_size - VC_HEADER_GROUP_SIZE;
	error = vc_read_header(fd, backup_offset, passphrase, passphrase_len,
	    iterations, device_size, mapping, volume_key, why, why_size);
	if (error == 0)
		return (0);
	if (error == 1)
		snprintf(why, why_size,
		    "no VeraCrypt header unlocked with this passphrase");

fail:
	explicit_bzero(volume_key, VC_VOLUME_KEY_SIZE);
	memset(mapping, 0, sizeof(*mapping));
	return (-1);
}
