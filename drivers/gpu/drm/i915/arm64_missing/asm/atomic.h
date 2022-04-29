#ifndef STUB_X86_ASM_ATOMIC_H
#define STUB_X86_ASM_ATOMIC_H

#include_next <asm/atomic.h>

#define try_cmpxchg(_ptr, _pold, _new)					\
({									\
	__typeof__(_ptr) _old = (__typeof__(_ptr))(_pold);		\
	__typeof__(*(_ptr)) __old = *_old;				\
	__typeof__(*(_ptr)) __cur = cmpxchg64(_ptr, __old, _new);	\
	bool success = __cur == __old;					\
	if (unlikely(!success))						\
		*_old = __cur;						\
	likely(success);						\
})

#endif
