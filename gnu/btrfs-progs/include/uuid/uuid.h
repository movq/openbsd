#ifndef BTRFS_UUID_COMPAT_H
#define BTRFS_UUID_COMPAT_H

typedef unsigned char uuid_t[16];

void uuid_generate(uuid_t out);
int uuid_is_null(const uuid_t uu);
int uuid_parse(const char *in, uuid_t uu);
void uuid_unparse(const uuid_t uu, char *out);

#endif
