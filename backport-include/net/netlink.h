#ifndef __BACKPORT_NET_NETLINK_H
#define __BACKPORT_NET_NETLINK_H
#include_next <net/netlink.h>
#include <linux/version.h>
#include <linux/in6.h>

#ifdef BPM_NLMSG_PARSE_NOT_PRESENT
/* can't backport using the enum - need to override */
#define NLA_UNSPEC		0
#define NLA_U8			1
#define NLA_U16			2
#define NLA_U32			3
#define NLA_U64			4
#define NLA_STRING		5
#define NLA_FLAG		6
#define NLA_MSECS		7
#define NLA_NESTED		8
#define NLA_NESTED_ARRAY	9
#define NLA_NUL_STRING		10
#define NLA_BINARY		11
#define NLA_S8			12
#define NLA_S16			13
#define NLA_S32			14
#define NLA_S64			15
#define NLA_BITFIELD32		16
#define NLA_REJECT		17
#define NLA_EXACT_LEN		18
#define NLA_EXACT_LEN_WARN	19
#define NLA_MIN_LEN		20
#define __NLA_TYPE_MAX		21
#define NLA_TYPE_MAX		(__NLA_TYPE_MAX - 1)

struct backport_nla_policy {
	u8		type;
	u8		validation_type;
	u16		len;
	union {
		const void *validation_data;
		struct {
			s16 min, max;
		};
		int (*validate)(const struct nlattr *attr,
				struct netlink_ext_ack *extack);
		u16 strict_start_type;
	};
};
#define nla_policy backport_nla_policy

#define nla_nest_start_noflag LINUX_I915_BACKPORT(nla_nest_start_noflag)
static inline struct nlattr *nla_nest_start_noflag(struct sk_buff *skb,
						   int attrtype)
{
	struct nlattr *start = (struct nlattr *)skb_tail_pointer(skb);

	if (nla_put(skb, attrtype, 0, NULL) < 0)
		return NULL;

	return start;
}

#define nla_nest_start LINUX_I915_BACKPORT(nla_nest_start)
static inline struct nlattr *nla_nest_start(struct sk_buff *skb, int attrtype)
{
	return nla_nest_start_noflag(skb, attrtype | NLA_F_NESTED);
}

enum netlink_validation {
	NL_VALIDATE_LIBERAL = 0,
	NL_VALIDATE_TRAILING = BIT(0),
	NL_VALIDATE_MAXTYPE = BIT(1),
	NL_VALIDATE_UNSPEC = BIT(2),
	NL_VALIDATE_STRICT_ATTRS = BIT(3),
	NL_VALIDATE_NESTED = BIT(4),
};

#define NL_VALIDATE_DEPRECATED_STRICT (NL_VALIDATE_TRAILING |\
				       NL_VALIDATE_MAXTYPE)
#define NL_VALIDATE_STRICT (NL_VALIDATE_TRAILING |\
			    NL_VALIDATE_MAXTYPE |\
			    NL_VALIDATE_UNSPEC |\
			    NL_VALIDATE_STRICT_ATTRS |\
			    NL_VALIDATE_NESTED)

#define __nla_validate LINUX_I915_BACKPORT(__nla_validate)
int __nla_validate(const struct nlattr *head, int len, int maxtype,
		   const struct nla_policy *policy, unsigned int validate,
		   struct netlink_ext_ack *extack);
#define __nla_parse LINUX_I915_BACKPORT(__nla_parse)
int __nla_parse(struct nlattr **tb, int maxtype, const struct nlattr *head,
		int len, const struct nla_policy *policy, unsigned int validate,
		struct netlink_ext_ack *extack);

#define nla_policy_len LINUX_I915_BACKPORT(nla_policy_len)
int nla_policy_len(const struct nla_policy *, int);

#define nla_parse LINUX_I915_BACKPORT(nla_parse)
static inline int nla_parse(struct nlattr **tb, int maxtype,
			    const struct nlattr *head, int len,
			    const struct nla_policy *policy,
			    struct netlink_ext_ack *extack)
{
	return __nla_parse(tb, maxtype, head, len, policy,
			   NL_VALIDATE_STRICT, extack);
}

#define nla_parse_deprecated LINUX_I915_BACKPORT(nla_parse_deprecated)
static inline int nla_parse_deprecated(struct nlattr **tb, int maxtype,
				       const struct nlattr *head, int len,
				       const struct nla_policy *policy,
				       struct netlink_ext_ack *extack)
{
	return __nla_parse(tb, maxtype, head, len, policy,
			   NL_VALIDATE_LIBERAL, extack);
}

#define nla_parse_deprecated_strict LINUX_I915_BACKPORT(nla_parse_deprecated_strict)
static inline int nla_parse_deprecated_strict(struct nlattr **tb, int maxtype,
					      const struct nlattr *head,
					      int len,
					      const struct nla_policy *policy,
					      struct netlink_ext_ack *extack)
{
	return __nla_parse(tb, maxtype, head, len, policy,
			   NL_VALIDATE_DEPRECATED_STRICT, extack);
}

#define __nlmsg_parse LINUX_I915_BACKPORT(__nlmsg_parse)
static inline int __nlmsg_parse(const struct nlmsghdr *nlh, int hdrlen,
				struct nlattr *tb[], int maxtype,
				const struct nla_policy *policy,
				unsigned int validate,
				struct netlink_ext_ack *extack)
{
	if (nlh->nlmsg_len < nlmsg_msg_size(hdrlen)) {
		NL_SET_ERR_MSG(extack, "Invalid header length");
		return -EINVAL;
	}

	return __nla_parse(tb, maxtype, nlmsg_attrdata(nlh, hdrlen),
			   nlmsg_attrlen(nlh, hdrlen), policy, validate,
			   extack);
}

#define nlmsg_parse LINUX_I915_BACKPORT(nlmsg_parse)
static inline int nlmsg_parse(const struct nlmsghdr *nlh, int hdrlen,
			      struct nlattr *tb[], int maxtype,
			      const struct nla_policy *policy,
			      struct netlink_ext_ack *extack)
{
	return __nla_parse(tb, maxtype, nlmsg_attrdata(nlh, hdrlen),
			   nlmsg_attrlen(nlh, hdrlen), policy,
			   NL_VALIDATE_STRICT, extack);
}

#define nlmsg_parse_deprecated LINUX_I915_BACKPORT(nlmsg_parse_deprecated)
static inline int nlmsg_parse_deprecated(const struct nlmsghdr *nlh, int hdrlen,
					 struct nlattr *tb[], int maxtype,
					 const struct nla_policy *policy,
					 struct netlink_ext_ack *extack)
{
	return __nlmsg_parse(nlh, hdrlen, tb, maxtype, policy,
			     NL_VALIDATE_LIBERAL, extack);
}

#define nlmsg_parse_deprecated_strict LINUX_I915_BACKPORT(nlmsg_parse_deprecated_strict)
static inline int
nlmsg_parse_deprecated_strict(const struct nlmsghdr *nlh, int hdrlen,
			      struct nlattr *tb[], int maxtype,
			      const struct nla_policy *policy,
			      struct netlink_ext_ack *extack)
{
	return __nlmsg_parse(nlh, hdrlen, tb, maxtype, policy,
			     NL_VALIDATE_DEPRECATED_STRICT, extack);
}

#define nla_validate_deprecated LINUX_I915_BACKPORT(nla_validate_deprecated)
static inline int nla_validate_deprecated(const struct nlattr *head, int len,
					  int maxtype,
					  const struct nla_policy *policy,
					  struct netlink_ext_ack *extack)
{
	return __nla_validate(head, len, maxtype, policy, NL_VALIDATE_LIBERAL,
			      extack);
}

#define nla_validate LINUX_I915_BACKPORT(nla_validate)
static inline int nla_validate(const struct nlattr *head, int len, int maxtype,
			       const struct nla_policy *policy,
			       struct netlink_ext_ack *extack)
{
	return __nla_validate(head, len, maxtype, policy, NL_VALIDATE_STRICT,
			      extack);
}

#define nlmsg_validate_deprecated LINUX_I915_BACKPORT(nlmsg_validate_deprecated)
static inline int nlmsg_validate_deprecated(const struct nlmsghdr *nlh,
					    int hdrlen, int maxtype,
					    const struct nla_policy *policy,
					    struct netlink_ext_ack *extack)
{
	if (nlh->nlmsg_len < nlmsg_msg_size(hdrlen))
		return -EINVAL;

	return __nla_validate(nlmsg_attrdata(nlh, hdrlen),
			      nlmsg_attrlen(nlh, hdrlen), maxtype,
			      policy, NL_VALIDATE_LIBERAL, extack);
}

#define nlmsg_validate LINUX_I915_BACKPORT(nlmsg_validate)
static inline int nlmsg_validate(const struct nlmsghdr *nlh,
				 int hdrlen, int maxtype,
				 const struct nla_policy *policy,
				 struct netlink_ext_ack *extack)
{
	if (nlh->nlmsg_len < nlmsg_msg_size(hdrlen))
		return -EINVAL;

	return __nla_validate(nlmsg_attrdata(nlh, hdrlen),
			      nlmsg_attrlen(nlh, hdrlen), maxtype,
			      policy, NL_VALIDATE_STRICT, extack);
}

#define nla_parse_nested LINUX_I915_BACKPORT(nla_parse_nested)
static inline int nla_parse_nested(struct nlattr *tb[], int maxtype,
				   const struct nlattr *nla,
				   const struct nla_policy *policy,
				   struct netlink_ext_ack *extack)
{
	if (!(nla->nla_type & NLA_F_NESTED)) {
		NL_SET_ERR_MSG_ATTR(extack, nla, "NLA_F_NESTED is missing");
		return -EINVAL;
	}

	return __nla_parse(tb, maxtype, nla_data(nla), nla_len(nla), policy,
			   NL_VALIDATE_STRICT, extack);
}

#define nla_parse_nested_deprecated LINUX_I915_BACKPORT(nla_parse_nested_deprecated)
static inline int nla_parse_nested_deprecated(struct nlattr *tb[], int maxtype,
					      const struct nlattr *nla,
					      const struct nla_policy *policy,
					      struct netlink_ext_ack *extack)
{
	return __nla_parse(tb, maxtype, nla_data(nla), nla_len(nla), policy,
			   NL_VALIDATE_LIBERAL, extack);
}

#define __nla_validate_nested LINUX_I915_BACKPORT(__nla_validate_nested)
static inline int __nla_validate_nested(const struct nlattr *start, int maxtype,
					const struct nla_policy *policy,
					unsigned int validate,
					struct netlink_ext_ack *extack)
{
	return __nla_validate(nla_data(start), nla_len(start), maxtype, policy,
			      validate, extack);
}

#define nla_validate_nested_deprecated LINUX_I915_BACKPORT(nla_validate_nested_deprecated)
static inline int
nla_validate_nested_deprecated(const struct nlattr *start, int maxtype,
			       const struct nla_policy *policy,
			       struct netlink_ext_ack *extack)
{
	return __nla_validate_nested(start, maxtype, policy,
				     NL_VALIDATE_LIBERAL, extack);
}
#endif /* < 5.2 */

#ifdef BPM_NLA_VALIDATE_NESTED_NOT_PRESENT 
#define nla_validate_nested LINUX_I915_BACKPORT(nla_validate_nested)
static inline int
nla_validate_nested(const struct nlattr *start, int maxtype,
		    const struct nla_policy *policy,
		    struct netlink_ext_ack *extack)
{
	return __nla_validate_nested(start, maxtype, policy,
				     NL_VALIDATE_STRICT, extack);
}
#endif

#ifdef  BPM_NLA_POLICY_NESTED_ARRAY_NOT_PRESENT
#undef NLA_POLICY_NESTED
#undef NLA_POLICY_NESTED_ARRAY
#define _NLA_POLICY_NESTED(maxattr, policy) \
	{ .type = NLA_NESTED, .validation_data = policy, .len = maxattr }
#define _NLA_POLICY_NESTED_ARRAY(maxattr, policy) \
	{ .type = NLA_NESTED_ARRAY, .validation_data = policy, .len = maxattr }
#define NLA_POLICY_NESTED(policy) \
	_NLA_POLICY_NESTED(ARRAY_SIZE(policy) - 1, policy)
#define NLA_POLICY_NESTED_ARRAY(policy) \
	_NLA_POLICY_NESTED_ARRAY(ARRAY_SIZE(policy) - 1, policy)
#endif 

#ifdef BPM_NLA_POLICY_VALIDATION_PRESENT
enum nla_policy_validation {
	NLA_VALIDATE_NONE,
	NLA_VALIDATE_RANGE,
	NLA_VALIDATE_MIN,
	NLA_VALIDATE_MAX,
	NLA_VALIDATE_FUNCTION,
};

#define NLA_POLICY_EXACT_LEN(_len)	{ .type = NLA_EXACT_LEN, .len = _len }
#define NLA_POLICY_EXACT_LEN_WARN(_len)	{ .type = NLA_EXACT_LEN_WARN, \
					  .len = _len }

#define NLA_POLICY_ETH_ADDR		NLA_POLICY_EXACT_LEN(ETH_ALEN)
#define NLA_POLICY_ETH_ADDR_COMPAT	NLA_POLICY_EXACT_LEN_WARN(ETH_ALEN)

#define __NLA_ENSURE(condition) (sizeof(char[1 - 2*!(condition)]) - 1)
#define NLA_ENSURE_INT_TYPE(tp)				\
	(__NLA_ENSURE(tp == NLA_S8 || tp == NLA_U8 ||	\
		      tp == NLA_S16 || tp == NLA_U16 ||	\
		      tp == NLA_S32 || tp == NLA_U32 ||	\
		      tp == NLA_S64 || tp == NLA_U64) + tp)
#define NLA_ENSURE_NO_VALIDATION_PTR(tp)		\
	(__NLA_ENSURE(tp != NLA_BITFIELD32 &&		\
		      tp != NLA_REJECT &&		\
		      tp != NLA_NESTED &&		\
		      tp != NLA_NESTED_ARRAY) + tp)

#define NLA_POLICY_RANGE(tp, _min, _max) {		\
	.type = NLA_ENSURE_INT_TYPE(tp),		\
	.validation_type = NLA_VALIDATE_RANGE,		\
	.min = _min,					\
	.max = _max					\
}

#define NLA_POLICY_MIN(tp, _min) {			\
	.type = NLA_ENSURE_INT_TYPE(tp),		\
	.validation_type = NLA_VALIDATE_MIN,		\
	.min = _min,					\
}

#define NLA_POLICY_MAX(tp, _max) {			\
	.type = NLA_ENSURE_INT_TYPE(tp),		\
	.validation_type = NLA_VALIDATE_MAX,		\
	.max = _max,					\
}

#define NLA_POLICY_VALIDATE_FN(tp, fn, ...) {		\
	.type = NLA_ENSURE_NO_VALIDATION_PTR(tp),	\
	.validation_type = NLA_VALIDATE_FUNCTION,	\
	.validate = fn,					\
	.len = __VA_ARGS__ + 0,				\
}
#endif /* < 4.20 */

#if LINUX_VERSION_IS_LESS(4,12,0)
#include <backport/magic.h>

static inline int _nla_validate5(const struct nlattr *head,
				 int len, int maxtype,
				 const struct nla_policy *policy,
				 struct netlink_ext_ack *extack)
{
	return nla_validate(head, len, maxtype, policy, extack);
}
static inline int _nla_validate4(const struct nlattr *head,
				 int len, int maxtype,
				 const struct nla_policy *policy)
{
	return nla_validate(head, len, maxtype, policy, NULL);
}
#undef nla_validate
#define nla_validate(...) \
	macro_dispatcher(_nla_validate, __VA_ARGS__)(__VA_ARGS__)

static inline int _nla_parse6(struct nlattr **tb, int maxtype,
			      const struct nlattr *head,
			      int len, const struct nla_policy *policy,
			      struct netlink_ext_ack *extack)
{
	return nla_parse(tb, maxtype, head, len, policy, extack);
}
static inline int _nla_parse5(struct nlattr **tb, int maxtype,
			      const struct nlattr *head,
			      int len, const struct nla_policy *policy)
{
	return nla_parse(tb, maxtype, head, len, policy, NULL);
}
#undef nla_parse
#define nla_parse(...) \
	macro_dispatcher(_nla_parse, __VA_ARGS__)(__VA_ARGS__)

static inline int _nlmsg_parse6(const struct nlmsghdr *nlh, int hdrlen,
			        struct nlattr *tb[], int maxtype,
			        const struct nla_policy *policy,
			        struct netlink_ext_ack *extack)
{
	return nlmsg_parse(nlh, hdrlen, tb, maxtype, policy, extack);
}
static inline int _nlmsg_parse5(const struct nlmsghdr *nlh, int hdrlen,
			        struct nlattr *tb[], int maxtype,
			        const struct nla_policy *policy)
{
	return nlmsg_parse(nlh, hdrlen, tb, maxtype, policy, NULL);
}
#undef nlmsg_parse
#define nlmsg_parse(...) \
	macro_dispatcher(_nlmsg_parse, __VA_ARGS__)(__VA_ARGS__)

static inline int _nlmsg_validate5(const struct nlmsghdr *nlh,
				   int hdrlen, int maxtype,
				   const struct nla_policy *policy,
				   struct netlink_ext_ack *extack)
{
	return nlmsg_validate(nlh, hdrlen, maxtype, policy, extack);
}
static inline int _nlmsg_validate4(const struct nlmsghdr *nlh,
				   int hdrlen, int maxtype,
				   const struct nla_policy *policy)
{
	return nlmsg_validate(nlh, hdrlen, maxtype, policy, NULL);
}
#undef nlmsg_validate
#define nlmsg_validate(...) \
	macro_dispatcher(_nlmsg_validate, __VA_ARGS__)(__VA_ARGS__)

static inline int _nla_parse_nested5(struct nlattr *tb[], int maxtype,
				     const struct nlattr *nla,
				     const struct nla_policy *policy,
				     struct netlink_ext_ack *extack)
{
	return nla_parse_nested(tb, maxtype, nla, policy, extack);
}
static inline int _nla_parse_nested4(struct nlattr *tb[], int maxtype,
				     const struct nlattr *nla,
				     const struct nla_policy *policy)
{
	return nla_parse_nested(tb, maxtype, nla, policy, NULL);
}
#undef nla_parse_nested
#define nla_parse_nested(...) \
	macro_dispatcher(_nla_parse_nested, __VA_ARGS__)(__VA_ARGS__)
#endif /* LINUX_VERSION_IS_LESS(4,12,0) */

#if LINUX_VERSION_IS_LESS(3,7,0)
/**
 * nla_put_s8 - Add a s8 netlink attribute to a socket buffer
 * @skb: socket buffer to add attribute to
 * @attrtype: attribute type
 * @value: numeric value
 */
#define nla_put_s8 LINUX_I915_BACKPORT(nla_put_s8)
static inline int nla_put_s8(struct sk_buff *skb, int attrtype, s8 value)
{
	return nla_put(skb, attrtype, sizeof(s8), &value);
}

/**
 * nla_put_s16 - Add a s16 netlink attribute to a socket buffer
 * @skb: socket buffer to add attribute to
 * @attrtype: attribute type
 * @value: numeric value
 */
#define nla_put_s16 LINUX_I915_BACKPORT(nla_put_s16)
static inline int nla_put_s16(struct sk_buff *skb, int attrtype, s16 value)
{
	return nla_put(skb, attrtype, sizeof(s16), &value);
}

/**
 * nla_put_s32 - Add a s32 netlink attribute to a socket buffer
 * @skb: socket buffer to add attribute to
 * @attrtype: attribute type
 * @value: numeric value
 */
#define nla_put_s32 LINUX_I915_BACKPORT(nla_put_s32)
static inline int nla_put_s32(struct sk_buff *skb, int attrtype, s32 value)
{
	return nla_put(skb, attrtype, sizeof(s32), &value);
}

/**
 * nla_get_s32 - return payload of s32 attribute
 * @nla: s32 netlink attribute
 */
#define nla_get_s32 LINUX_I915_BACKPORT(nla_get_s32)
static inline s32 nla_get_s32(const struct nlattr *nla)
{
	return *(s32 *) nla_data(nla);
}

/**
 * nla_get_s16 - return payload of s16 attribute
 * @nla: s16 netlink attribute
 */
#define nla_get_s16 LINUX_I915_BACKPORT(nla_get_s16)
static inline s16 nla_get_s16(const struct nlattr *nla)
{
	return *(s16 *) nla_data(nla);
}

/**
 * nla_get_s8 - return payload of s8 attribute
 * @nla: s8 netlink attribute
 */
#define nla_get_s8 LINUX_I915_BACKPORT(nla_get_s8)
static inline s8 nla_get_s8(const struct nlattr *nla)
{
	return *(s8 *) nla_data(nla);
}

/**
 * nla_get_s64 - return payload of s64 attribute
 * @nla: s64 netlink attribute
 */
#define nla_get_s64 LINUX_I915_BACKPORT(nla_get_s64)
static inline s64 nla_get_s64(const struct nlattr *nla)
{
	s64 tmp;

	nla_memcpy(&tmp, nla, sizeof(tmp));

	return tmp;
}
#endif /* < 3.7.0 */

#if LINUX_VERSION_IS_LESS(3,5,0)
/*
 * This backports:
 * commit 569a8fc38367dfafd87454f27ac646c8e6b54bca
 * Author: David S. Miller <davem@davemloft.net>
 * Date:   Thu Mar 29 23:18:53 2012 -0400
 *
 *     netlink: Add nla_put_be{16,32,64}() helpers.
 */

#define nla_put_be16 LINUX_I915_BACKPORT(nla_put_be16)
static inline int nla_put_be16(struct sk_buff *skb, int attrtype, __be16 value)
{
	return nla_put(skb, attrtype, sizeof(__be16), &value);
}

#define nla_put_be32 LINUX_I915_BACKPORT(nla_put_be32)
static inline int nla_put_be32(struct sk_buff *skb, int attrtype, __be32 value)
{
	return nla_put(skb, attrtype, sizeof(__be32), &value);
}

#define nla_put_be64 LINUX_I915_BACKPORT(nla_put_be64)
static inline int nla_put_be64(struct sk_buff *skb, int attrtype, __be64 value)
{
	return nla_put(skb, attrtype, sizeof(__be64), &value);
}
#endif /* < 3.5 */

#if LINUX_VERSION_IS_LESS(3,7,0)
#define NLA_S8 (NLA_BINARY + 1)
#define NLA_S16 (NLA_BINARY + 2)
#define NLA_S32 (NLA_BINARY + 3)
#define NLA_S64 (NLA_BINARY + 4)
#define __NLA_TYPE_MAX (NLA_BINARY + 5)

#undef NLA_TYPE_MAX
#define NLA_TYPE_MAX (__NLA_TYPE_MAX - 1)
#endif

#if LINUX_VERSION_IS_LESS(4,1,0)
#define nla_put_in_addr LINUX_I915_BACKPORT(nla_put_in_addr)
static inline int nla_put_in_addr(struct sk_buff *skb, int attrtype,
				  __be32 addr)
{
	return nla_put_be32(skb, attrtype, addr);
}

#define nla_put_in6_addr LINUX_I915_BACKPORT(nla_put_in6_addr)
static inline int nla_put_in6_addr(struct sk_buff *skb, int attrtype,
				   const struct in6_addr *addr)
{
	return nla_put(skb, attrtype, sizeof(*addr), addr);
}

static inline __be32 nla_get_in_addr(const struct nlattr *nla)
{
	return *(__be32 *) nla_data(nla);
}

static inline struct in6_addr nla_get_in6_addr(const struct nlattr *nla)
{
	struct in6_addr tmp;

	nla_memcpy(&tmp, nla, sizeof(tmp));
	return tmp;
}
#endif /* < 4.1 */

#if LINUX_VERSION_IS_LESS(4,4,0)
/**
 * nla_get_le32 - return payload of __le32 attribute
 * @nla: __le32 netlink attribute
 */
#define nla_get_le32 LINUX_I915_BACKPORT(nla_get_le32)
static inline __le32 nla_get_le32(const struct nlattr *nla)
{
	return *(__le32 *) nla_data(nla);
}

/**
 * nla_get_le64 - return payload of __le64 attribute
 * @nla: __le64 netlink attribute
 */
#define nla_get_le64 LINUX_I915_BACKPORT(nla_get_le64)
static inline __le64 nla_get_le64(const struct nlattr *nla)
{
	return *(__le64 *) nla_data(nla);
}
#endif /* < 4.4 */

#if LINUX_VERSION_IS_LESS(4,7,0)
/**
 * nla_need_padding_for_64bit - test 64-bit alignment of the next attribute
 * @skb: socket buffer the message is stored in
 *
 * Return true if padding is needed to align the next attribute (nla_data()) to
 * a 64-bit aligned area.
 */
#define nla_need_padding_for_64bit LINUX_I915_BACKPORT(nla_need_padding_for_64bit)
static inline bool nla_need_padding_for_64bit(struct sk_buff *skb)
{
#ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
       /* The nlattr header is 4 bytes in size, that's why we test
        * if the skb->data _is_ aligned.  A NOP attribute, plus
        * nlattr header for next attribute, will make nla_data()
        * 8-byte aligned.
        */
       if (IS_ALIGNED((unsigned long)skb_tail_pointer(skb), 8))
               return true;
#endif
       return false;
}
/**
 * nla_align_64bit - 64-bit align the nla_data() of next attribute
 * @skb: socket buffer the message is stored in
 * @padattr: attribute type for the padding
 *
 * Conditionally emit a padding netlink attribute in order to make
 * the next attribute we emit have a 64-bit aligned nla_data() area.
 * This will only be done in architectures which do not have
 * HAVE_EFFICIENT_UNALIGNED_ACCESS defined.
 *
 * Returns zero on success or a negative error code.
 */
#define nla_align_64bit LINUX_I915_BACKPORT(nla_align_64bit)
static inline int nla_align_64bit(struct sk_buff *skb, int padattr)
{
       if (nla_need_padding_for_64bit(skb) &&
            !nla_reserve(skb, padattr, 0))
                return -EMSGSIZE;
       return 0;
}

/**
 * nla_total_size_64bit - total length of attribute including padding
 * @payload: length of payload
 */
#define nla_total_size_64bit LINUX_I915_BACKPORT(nla_total_size_64bit)
static inline int nla_total_size_64bit(int payload)
{
       return NLA_ALIGN(nla_attr_size(payload))
#ifndef HAVE_EFFICIENT_UNALIGNED_ACCESS
               + NLA_ALIGN(nla_attr_size(0))
#endif
               ;
}
#define __nla_reserve_64bit LINUX_I915_BACKPORT(__nla_reserve_64bit)
struct nlattr *__nla_reserve_64bit(struct sk_buff *skb, int attrtype,
				   int attrlen, int padattr);
#define nla_reserve_64bit LINUX_I915_BACKPORT(nla_reserve_64bit)
struct nlattr *nla_reserve_64bit(struct sk_buff *skb, int attrtype,
				 int attrlen, int padattr);
#define __nla_put_64bit LINUX_I915_BACKPORT(__nla_put_64bit)
void __nla_put_64bit(struct sk_buff *skb, int attrtype, int attrlen,
		     const void *data, int padattr);
#define nla_put_64bit LINUX_I915_BACKPORT(nla_put_64bit)
int nla_put_64bit(struct sk_buff *skb, int attrtype, int attrlen,
		  const void *data, int padattr);
/**
 * nla_put_u64_64bit - Add a u64 netlink attribute to a skb and align it
 * @skb: socket buffer to add attribute to
 * @attrtype: attribute type
 * @value: numeric value
 * @padattr: attribute type for the padding
 */
#define nla_put_u64_64bit LINUX_I915_BACKPORT(nla_put_u64_64bit)
static inline int nla_put_u64_64bit(struct sk_buff *skb, int attrtype,
                                    u64 value, int padattr)
{
        return nla_put_64bit(skb, attrtype, sizeof(u64), &value, padattr);
}


/**
 * nla_put_s64 - Add a s64 netlink attribute to a socket buffer and align it
 * @skb: socket buffer to add attribute to
 * @attrtype: attribute type
 * @value: numeric value
 * @padattr: attribute type for the padding
 */
#define nla_put_s64 LINUX_I915_BACKPORT(nla_put_s64)
static inline int nla_put_s64(struct sk_buff *skb, int attrtype, s64 value,
			      int padattr)
{
	return nla_put_64bit(skb, attrtype, sizeof(s64), &value, padattr);
}
#endif /* < 4.7 */

#if LINUX_VERSION_IS_LESS(4,10,0)
/**
 * nla_memdup - duplicate attribute memory (kmemdup)
 * @src: netlink attribute to duplicate from
 * @gfp: GFP mask
 */
#define nla_memdump LINUX_I915_BACKPORT(nla_memdup)
static inline void *nla_memdup(const struct nlattr *src, gfp_t gfp)
{
	return kmemdup(nla_data(src), nla_len(src), gfp);
}
#endif /* < 4.10 */

#endif /* __BACKPORT_NET_NETLINK_H */
