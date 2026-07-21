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

#ifndef VERACRYPT_H
#define VERACRYPT_H

#include <sys/types.h>

#include <stdint.h>

#define VC_VOLUME_KEY_SIZE	64
#define VC_PASSWORD_MAX		128
#define VC_PIM_MAX		2147468

struct vc_mapping {
	uint64_t	data_offset;
	uint64_t	data_length;
	uint64_t	iv_offset;
	uint32_t	sector_size;
	uint32_t	xts_sector_size;
};

int	vc_read_unlock(int, uint64_t, const char *, size_t, uint32_t,
	    struct vc_mapping *, uint8_t[VC_VOLUME_KEY_SIZE], char *, size_t);

#endif
