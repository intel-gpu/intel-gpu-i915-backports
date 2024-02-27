#ifndef __BACKPORT_LINUX_UUID_H_
#define __BACKPORT_LINUX_UUID_H_
#include <linux/version.h>
#include_next <linux/uuid.h>

#ifdef BPM_UUID_LE_CMP_NOT_PRESENT
#include <uapi/linux/mei_uuid.h>
static inline int uuid_le_cmp(const uuid_le u1, const uuid_le u2)
{
        return memcmp(&u1, &u2, sizeof(uuid_le));
}
#endif

#ifndef UUID_STRING_LEN
/*
 * The length of a UUID string ("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
 * not including trailing NUL.
 */
#define	UUID_STRING_LEN		36
#endif

#if LINUX_VERSION_IS_LESS(4,13,0)
#define guid_t uuid_le
#define uuid_t uuid_be

static inline void guid_gen(guid_t *u)
{
	return uuid_le_gen(u);
}
static inline void uuid_gen(uuid_t *u)
{
	return uuid_be_gen(u);
}

static inline void guid_copy(guid_t *dst, const guid_t *src)
{
	memcpy(dst, src, sizeof(guid_t));
}
#endif

#endif /* __BACKPORT_LINUX_UUID_H_ */
