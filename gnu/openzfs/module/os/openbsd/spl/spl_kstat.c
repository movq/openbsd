// SPDX-License-Identifier: BSD-2-Clause
/* OpenZFS kstats exported through the native OpenBSD kstat subsystem. */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kstat.h>
#include <sys/debug.h>
#include <sys/time.h>

static int
zfs_kstat_default_update(kstat_t *ksp, int rw)
{

	ASSERT3P(ksp, !=, NULL);
	return (rw == KSTAT_WRITE ? EACCES : 0);
}

static uint32_t
zfs_kstat_name_hash(const char *first, const char *second)
{
	const unsigned char *p;
	uint32_t hash = 2166136261U;

	for (p = (const unsigned char *)first; *p != '\0'; p++) {
		hash ^= *p;
		hash *= 16777619U;
	}
	if (second != NULL) {
		hash ^= '.';
		hash *= 16777619U;
		for (p = (const unsigned char *)second; *p != '\0'; p++) {
			hash ^= *p;
			hash *= 16777619U;
		}
	}
	return (hash);
}

static int
zfs_kstat_name_char(int ch)
{

	return ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
	    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.');
}

static void
zfs_kstat_native_name(char *dst, size_t size, const char *first,
    const char *second)
{
	char source[KSTAT_STRLEN * 2 + 2];
	size_t i, len, prefix;
	uint32_t hash;
	int changed = 0;

	if (second == NULL || *second == '\0')
		(void)snprintf(source, sizeof (source), "%s", first);
	else
		(void)snprintf(source, sizeof (source), "%s.%s", first, second);

	len = strlen(source);
	for (i = 0; i < len; i++) {
		if (!zfs_kstat_name_char((unsigned char)source[i])) {
			source[i] = '_';
			changed = 1;
		}
	}
	if (len == 0) {
		source[0] = 'z';
		source[1] = '\0';
		len = 1;
		changed = 1;
	}

	if (!changed && len < size) {
		(void)strlcpy(dst, source, size);
		return;
	}

	hash = zfs_kstat_name_hash(first, second);
	prefix = size > 10 ? size - 10 : 0;
	(void)snprintf(dst, size, "%.*s_%08x", (int)prefix, source, hash);
}

void
seq_printf(struct seq_file *f, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(f->sf_buf, f->sf_size, fmt, ap);
	va_end(ap);
}

void
zfs_kstat_set_raw_ops(kstat_t *ksp,
    int (*headers)(char *, size_t),
    int (*data)(char *, size_t, void *),
    void *(*addr)(kstat_t *, off_t))
{

	ASSERT3P(ksp, !=, NULL);
	ksp->ks_raw_ops.headers = headers;
	ksp->ks_raw_ops.data = data;
	ksp->ks_raw_ops.addr = addr;
}

void
zfs_kstat_set_seq_raw_ops(kstat_t *ksp,
    int (*headers)(struct seq_file *),
    int (*data)(char *, size_t, void *),
    void *(*addr)(kstat_t *, off_t))
{

	ASSERT3P(ksp, !=, NULL);
	ksp->ks_raw_ops.seq_headers = headers;
	ksp->ks_raw_ops.data = data;
	ksp->ks_raw_ops.addr = addr;
}

static void *
zfs_kstat_raw_default_addr(kstat_t *ksp, off_t index)
{

	return (index == 0 ? ksp->ks_data : NULL);
}

static int
zfs_kstat_raw_append(char *buf, size_t size, size_t *used,
    int (*render)(char *, size_t, void *), void *arg)
{
	size_t len, remain;
	int error;

	if (*used >= size)
		return (ENOMEM);
	remain = size - *used;
	buf[*used] = '\0';
	error = render(buf + *used, remain, arg);
	if (error != 0)
		return (error);
	len = strnlen(buf + *used, remain);
	if (len == remain)
		return (ENOMEM);
	*used += len;
	return (0);
}

static int
zfs_kstat_raw_snapshot(kstat_t *ksp, char *buf, size_t size, size_t *usedp)
{
	void *(*addr)(kstat_t *, off_t);
	void *data;
	size_t len, used = 0;
	off_t index;
	int error;

	if (size == 0)
		return (ENOMEM);
	buf[0] = '\0';

	if (!ISSET(ksp->ks_flags, KSTAT_FLAG_NO_HEADERS)) {
		if (ksp->ks_raw_ops.headers != NULL) {
			error = ksp->ks_raw_ops.headers(buf, size);
			if (error != 0)
				return (error);
		} else if (ksp->ks_raw_ops.seq_headers != NULL) {
			struct seq_file f = { buf, size };

			error = ksp->ks_raw_ops.seq_headers(&f);
			if (error != 0)
				return (error);
		}
		used = strnlen(buf, size);
		if (used == size)
			return (ENOMEM);
		if (used != 0 && buf[used - 1] != '\n') {
			if (used + 1 >= size)
				return (ENOMEM);
			buf[used++] = '\n';
			buf[used] = '\0';
		}
	}

	addr = ksp->ks_raw_ops.addr != NULL ? ksp->ks_raw_ops.addr :
	    zfs_kstat_raw_default_addr;
	for (index = 0; (data = addr(ksp, index)) != NULL; index++) {
		if (ksp->ks_raw_ops.data == NULL)
			return (EINVAL);
		error = zfs_kstat_raw_append(buf, size, &used,
		    ksp->ks_raw_ops.data, data);
		if (error != 0)
			return (error);
		if (index == (off_t)UINT_MAX)
			return (EOVERFLOW);
	}

	len = used == 0 ? 1 : used + 1;
	*usedp = len;
	return (0);
}

static int
zfs_kstat_named_is_native(kstat_t *ksp)
{
	kstat_named_t *named = ksp->ks_data;
	uint_t i, j;

	if (named == NULL || ksp->ks_ndata == 0)
		return (0);
	for (i = 0; i < ksp->ks_ndata; i++) {
		if (named[i].name[0] == '\0' ||
		    strnlen(named[i].name, KSTAT_STRLEN) >= KSTAT_KV_NAMELEN ||
		    named[i].data_type >= KSTAT_NUM_DATAS ||
		    named[i].data_type == KSTAT_DATA_STRING)
			return (0);
		for (j = 0; j < i; j++) {
			if (strcmp(named[i].name, named[j].name) == 0)
				return (0);
		}
	}
	return (1);
}

static enum kstat_kv_type
zfs_kstat_named_type(uchar_t type)
{

	switch (type) {
	case KSTAT_DATA_CHAR:
		return (KSTAT_KV_T_ISTR);
	case KSTAT_DATA_INT32:
		return (KSTAT_KV_T_INT32);
	case KSTAT_DATA_UINT32:
		return (KSTAT_KV_T_UINT32);
	case KSTAT_DATA_INT64:
	case KSTAT_DATA_LONG:
		return (KSTAT_KV_T_INT64);
	case KSTAT_DATA_UINT64:
	case KSTAT_DATA_ULONG:
		return (KSTAT_KV_T_UINT64);
	default:
		panic("unsupported named kstat type %u", type);
	}
}

static void
zfs_kstat_named_snapshot(kstat_t *ksp)
{
	kstat_named_t *src = ksp->ks_data;
	struct kstat_kv *dst = ksp->ks_native_data;
	uint_t i;

	for (i = 0; i < ksp->ks_ndata; i++) {
		kstat_kv_init(&dst[i], src[i].name,
		    zfs_kstat_named_type(src[i].data_type));
		switch (src[i].data_type) {
		case KSTAT_DATA_CHAR:
			memcpy(kstat_kv_istr(&dst[i]), src[i].value.c,
			    sizeof (dst[i].kv_v.v_istr));
			kstat_kv_istr(&dst[i])[
			    sizeof (dst[i].kv_v.v_istr) - 1] = '\0';
			break;
		case KSTAT_DATA_INT32:
			kstat_kv_s32(&dst[i]) = src[i].value.i32;
			break;
		case KSTAT_DATA_UINT32:
			kstat_kv_u32(&dst[i]) = src[i].value.ui32;
			break;
		case KSTAT_DATA_INT64:
			kstat_kv_s64(&dst[i]) = src[i].value.i64;
			break;
		case KSTAT_DATA_UINT64:
			kstat_kv_u64(&dst[i]) = src[i].value.ui64;
			break;
		case KSTAT_DATA_LONG:
			kstat_kv_s64(&dst[i]) = src[i].value.l;
			break;
		case KSTAT_DATA_ULONG:
			kstat_kv_u64(&dst[i]) = src[i].value.ul;
			break;
		default:
			panic("unsupported named kstat type %u",
			    src[i].data_type);
		}
	}
}

static size_t
zfs_kstat_named_value_size(const kstat_named_t *named)
{

	switch (named->data_type) {
	case KSTAT_DATA_CHAR:
		return (sizeof (named->value.c));
	case KSTAT_DATA_INT32:
		return (sizeof (named->value.i32));
	case KSTAT_DATA_UINT32:
		return (sizeof (named->value.ui32));
	case KSTAT_DATA_INT64:
		return (sizeof (named->value.i64));
	case KSTAT_DATA_UINT64:
		return (sizeof (named->value.ui64));
	case KSTAT_DATA_LONG:
		return (sizeof (named->value.l));
	case KSTAT_DATA_ULONG:
		return (sizeof (named->value.ul));
	case KSTAT_DATA_STRING:
		return (KSTAT_NAMED_STR_BUFLEN(named));
	default:
		return (0);
	}
}

static size_t
zfs_kstat_named_raw_size(kstat_t *ksp)
{
	kstat_named_t *named = ksp->ks_data;
	size_t name_size, value_size;
	size_t size = sizeof (struct zfs_kstat_named_raw_header);
	uint_t i;

	if (named == NULL)
		return (0);
	for (i = 0; i < ksp->ks_ndata; i++) {
		if (named[i].data_type >= KSTAT_NUM_DATAS)
			return (0);
		name_size = strnlen(named[i].name, KSTAT_STRLEN);
		if (name_size == KSTAT_STRLEN)
			return (0);
		name_size++;
		value_size = zfs_kstat_named_value_size(&named[i]);
		if (named[i].data_type == KSTAT_DATA_STRING &&
		    value_size != 0 && KSTAT_NAMED_STR_PTR(&named[i]) == NULL)
			return (0);
		if (value_size > KSTAT_RAW_MAX)
			return (0);
		if (size > KSTAT_RAW_MAX -
		    sizeof (struct zfs_kstat_named_raw_record) ||
		    size + sizeof (struct zfs_kstat_named_raw_record) >
		    KSTAT_RAW_MAX - name_size ||
		    size + sizeof (struct zfs_kstat_named_raw_record) +
		    name_size > KSTAT_RAW_MAX - value_size)
			return (0);
		size += sizeof (struct zfs_kstat_named_raw_record) +
		    name_size + value_size;
	}
	return (size);
}

static int
zfs_kstat_named_raw_snapshot(kstat_t *ksp)
{
	struct zfs_kstat_named_raw_header *header = ksp->ks_native_data;
	struct zfs_kstat_named_raw_record *record;
	kstat_named_t *named = ksp->ks_data;
	char *out = (char *)(header + 1);
	size_t name_size, value_size;
	uint_t i;

	if (zfs_kstat_named_raw_size(ksp) != ksp->ks_native_size)
		return (ENOMEM);
	header->znrh_version = ZFS_KSTAT_NAMED_RAW_VERSION;
	header->znrh_count = ksp->ks_ndata;
	for (i = 0; i < ksp->ks_ndata; i++) {
		name_size = strlen(named[i].name) + 1;
		value_size = zfs_kstat_named_value_size(&named[i]);
		record = (struct zfs_kstat_named_raw_record *)out;
		record->znrr_name_size = name_size;
		record->znrr_data_type = named[i].data_type;
		record->znrr_reserved = 0;
		record->znrr_data_size = value_size;
		out += sizeof (*record);
		memcpy(out, named[i].name, name_size);
		out += name_size;
		switch (named[i].data_type) {
		case KSTAT_DATA_CHAR:
			memcpy(out, named[i].value.c, value_size);
			break;
		case KSTAT_DATA_INT32:
			memcpy(out, &named[i].value.i32, value_size);
			break;
		case KSTAT_DATA_UINT32:
			memcpy(out, &named[i].value.ui32, value_size);
			break;
		case KSTAT_DATA_INT64:
			memcpy(out, &named[i].value.i64, value_size);
			break;
		case KSTAT_DATA_UINT64:
			memcpy(out, &named[i].value.ui64, value_size);
			break;
		case KSTAT_DATA_LONG:
			memcpy(out, &named[i].value.l, value_size);
			break;
		case KSTAT_DATA_ULONG:
			memcpy(out, &named[i].value.ul, value_size);
			break;
		case KSTAT_DATA_STRING:
			if (value_size != 0)
				memcpy(out, KSTAT_NAMED_STR_PTR(&named[i]),
				    value_size);
			break;
		default:
			return (EINVAL);
		}
		out += value_size;
	}
	return (0);
}

static int
zfs_kstat_native_read(struct kstat *native)
{
	kstat_t *ksp = native->ks_softc;
	size_t used;
	int error;

	mutex_enter(ksp->ks_lock);
	error = ksp->ks_update(ksp, KSTAT_READ);
	if (error != 0)
		goto out;

	switch (ksp->ks_snapshot_type) {
	case ZFS_KSTAT_SNAPSHOT_KV:
		zfs_kstat_named_snapshot(ksp);
		break;
	case ZFS_KSTAT_SNAPSHOT_NAMED_RAW:
		error = zfs_kstat_named_raw_snapshot(ksp);
		break;
	case ZFS_KSTAT_SNAPSHOT_RAW:
		if (ksp->ks_data_size > ksp->ks_native_size) {
			error = ENOMEM;
			break;
		}
		if (ksp->ks_data_size != 0)
			memcpy(ksp->ks_native_data, ksp->ks_data,
			    ksp->ks_data_size);
		if (ksp->ks_data_size < ksp->ks_native_size)
			memset((char *)ksp->ks_native_data +
			    ksp->ks_data_size, 0,
			    ksp->ks_native_size - ksp->ks_data_size);
		break;
	case ZFS_KSTAT_SNAPSHOT_FORMATTED:
		error = zfs_kstat_raw_snapshot(ksp, ksp->ks_native_data,
		    ksp->ks_native_size, &used);
		if (error == 0 && used < ksp->ks_native_size)
			memset((char *)ksp->ks_native_data + used, 0,
			    ksp->ks_native_size - used);
		break;
	default:
		panic("kstat %p has no snapshot type", ksp);
	}

	ksp->ks_snaptime = gethrtime();
	getnanouptime(&native->ks_updated);
out:
	mutex_exit(ksp->ks_lock);
	return (error);
}

kstat_t *
zfs_kstat_create(const char *module, int instance, const char *name,
    const char *class, uchar_t type, uint_t ndata, uchar_t flags)
{
	kstat_t *ksp;
	size_t size;

	ASSERT3P(module, !=, NULL);
	ASSERT3P(name, !=, NULL);
	ASSERT3S(instance, >=, 0);
	ASSERT3U(type, <, KSTAT_NUM_TYPES);
	if (class == NULL)
		class = "misc";

	switch (type) {
	case KSTAT_TYPE_RAW:
		size = ndata;
		ndata = 1;
		break;
	case KSTAT_TYPE_NAMED:
		VERIFY3U(ndata, <=, (size_t)-1 / sizeof (kstat_named_t));
		size = ndata * sizeof (kstat_named_t);
		break;
	case KSTAT_TYPE_INTR:
		VERIFY3U(ndata, ==, 1);
		size = sizeof (kstat_intr_t);
		break;
	case KSTAT_TYPE_IO:
		VERIFY3U(ndata, ==, 1);
		size = sizeof (kstat_io_t);
		break;
	case KSTAT_TYPE_TIMER:
		VERIFY3U(ndata, <=, (size_t)-1 / sizeof (kstat_timer_t));
		size = ndata * sizeof (kstat_timer_t);
		break;
	default:
		panic("invalid kstat type %u", type);
	}

	ksp = kmem_zalloc(sizeof (*ksp), KM_SLEEP);
	ksp->ks_magic = KS_MAGIC;
	ksp->ks_crtime = gethrtime();
	ksp->ks_snaptime = ksp->ks_crtime;
	(void)strlcpy(ksp->ks_module, module, sizeof (ksp->ks_module));
	ksp->ks_instance = instance;
	(void)strlcpy(ksp->ks_name, name, sizeof (ksp->ks_name));
	(void)strlcpy(ksp->ks_class, class, sizeof (ksp->ks_class));
	ksp->ks_type = type;
	ksp->ks_flags = flags;
	ksp->ks_ndata = ndata;
	ksp->ks_data_size = size;
	ksp->ks_update = zfs_kstat_default_update;

	mutex_init(&ksp->ks_private_lock, NULL, MUTEX_DEFAULT, NULL);
	ksp->ks_lock = &ksp->ks_private_lock;
	if (!ISSET(flags, KSTAT_FLAG_VIRTUAL) && size != 0)
		ksp->ks_data = kmem_zalloc(size, KM_SLEEP);

	return (ksp);
}

static int
zfs_kstat_prepare_snapshot(kstat_t *ksp)
{
	char *raw;
	size_t size, used;
	int error;

	if (ksp->ks_type == KSTAT_TYPE_NAMED &&
	    zfs_kstat_named_is_native(ksp)) {
		ksp->ks_snapshot_type = ZFS_KSTAT_SNAPSHOT_KV;
		ksp->ks_native_size = ksp->ks_ndata *
		    sizeof (struct kstat_kv);
		ksp->ks_native_data = kmem_zalloc(ksp->ks_native_size,
		    KM_SLEEP);
		zfs_kstat_named_snapshot(ksp);
		return (KSTAT_T_KV);
	}
	if (ksp->ks_type == KSTAT_TYPE_NAMED) {
		size = zfs_kstat_named_raw_size(ksp);
		if (size == 0)
			return (-EINVAL);
		ksp->ks_snapshot_type = ZFS_KSTAT_SNAPSHOT_NAMED_RAW;
		ksp->ks_native_size = size;
		ksp->ks_native_data = kmem_zalloc(size, KM_SLEEP);
		error = zfs_kstat_named_raw_snapshot(ksp);
		VERIFY3S(error, ==, 0);
		return (KSTAT_T_RAW);
	}

	if (ksp->ks_type != KSTAT_TYPE_RAW ||
	    (ksp->ks_raw_ops.headers == NULL &&
	    ksp->ks_raw_ops.seq_headers == NULL &&
	    ksp->ks_raw_ops.data == NULL)) {
		ksp->ks_snapshot_type = ZFS_KSTAT_SNAPSHOT_RAW;
		ksp->ks_native_size = MAX(ksp->ks_data_size, 1);
		ksp->ks_native_data = kmem_zalloc(ksp->ks_native_size,
		    KM_SLEEP);
		if (ksp->ks_data != NULL && ksp->ks_data_size != 0)
			memcpy(ksp->ks_native_data, ksp->ks_data,
			    ksp->ks_data_size);
		return (KSTAT_T_RAW);
	}

	raw = kmem_zalloc(KSTAT_RAW_MAX, KM_SLEEP);
	mutex_enter(ksp->ks_lock);
	error = ksp->ks_update(ksp, KSTAT_READ);
	if (error == 0)
		error = zfs_kstat_raw_snapshot(ksp, raw, KSTAT_RAW_MAX, &used);
	mutex_exit(ksp->ks_lock);
	if (error != 0) {
		kmem_free(raw, KSTAT_RAW_MAX);
		return (-error);
	}

	ksp->ks_snapshot_type = ZFS_KSTAT_SNAPSHOT_FORMATTED;
	ksp->ks_native_size = used;
	ksp->ks_native_data = kmem_alloc(used, KM_SLEEP);
	memcpy(ksp->ks_native_data, raw, used);
	kmem_free(raw, KSTAT_RAW_MAX);
	return (KSTAT_T_RAW);
}

void
zfs_kstat_install(kstat_t *ksp)
{
	struct kstat *native;
	int native_type;

	ASSERT3P(ksp, !=, NULL);
	ASSERT3U(ksp->ks_magic, ==, KS_MAGIC);
	ASSERT3P(ksp->ks_native, ==, NULL);

	native_type = zfs_kstat_prepare_snapshot(ksp);
	if (native_type < 0) {
		printf("zfs: cannot snapshot kstat %s:%s: error %d\n",
		    ksp->ks_module, ksp->ks_name, -native_type);
		return;
	}

	zfs_kstat_native_name(ksp->ks_native_provider,
	    sizeof (ksp->ks_native_provider), ksp->ks_module, ksp->ks_class);
	zfs_kstat_native_name(ksp->ks_native_name,
	    sizeof (ksp->ks_native_name), ksp->ks_name, NULL);

	/* Parentheses bypass the function-like OpenZFS compatibility macros. */
	native = (kstat_create)(ksp->ks_native_provider, ksp->ks_instance,
	    ksp->ks_native_name, 0, native_type, 0);
	if (native == NULL) {
		printf("zfs: native kstat collision for %s:%d:%s\n",
		    ksp->ks_native_provider, ksp->ks_instance,
		    ksp->ks_native_name);
		kmem_free(ksp->ks_native_data, ksp->ks_native_size);
		ksp->ks_native_data = NULL;
		ksp->ks_native_size = 0;
		ksp->ks_snapshot_type = ZFS_KSTAT_SNAPSHOT_NONE;
		return;
	}

	native->ks_softc = ksp;
	native->ks_data = ksp->ks_native_data;
	native->ks_datalen = ksp->ks_native_size;
	native->ks_read = zfs_kstat_native_read;
	(kstat_install)(native);
	ksp->ks_native = native;
	ksp->ks_kid = native->ks_id;
}

void
zfs_kstat_delete(kstat_t *ksp)
{

	if (ksp == NULL)
		return;
	ASSERT3U(ksp->ks_magic, ==, KS_MAGIC);

	if (ksp->ks_native != NULL) {
		(kstat_remove)(ksp->ks_native);
		(kstat_destroy)(ksp->ks_native);
		ksp->ks_native = NULL;
	}
	if (ksp->ks_native_data != NULL)
		kmem_free(ksp->ks_native_data, ksp->ks_native_size);
	if (!ISSET(ksp->ks_flags, KSTAT_FLAG_VIRTUAL) &&
	    ksp->ks_data != NULL)
		kmem_free(ksp->ks_data, ksp->ks_data_size);
	mutex_destroy(&ksp->ks_private_lock);
	ksp->ks_magic = 0;
	kmem_free(ksp, sizeof (*ksp));
}

int
spl_kstat_init(void)
{

	return (0);
}

void
spl_kstat_fini(void)
{
}
