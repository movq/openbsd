// SPDX-License-Identifier: CDDL-1.0
/*
 * Copyright (c) 2014 by Chunwei Chen. All rights reserved.
 * Copyright (c) 2016, 2019 by Delphix. All rights reserved.
 */

#ifndef _ABD_OS_H
#define _ABD_OS_H

struct abd;

/*
 * Scattered ABD data is stored in independently allocated, directly mapped
 * PAGE_SIZE chunks.  The array is variable-sized and belongs to the abd_t.
 */
struct abd_scatter {
	uint_t		abd_offset;
	void		*abd_chunks[1];
};

struct abd_linear {
	void		*abd_buf;
};

#endif /* _ABD_OS_H */
