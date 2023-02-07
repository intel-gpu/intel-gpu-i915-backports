#ifndef __BACKPORT_LINUX_IOSYS_MAP_H
#define __BACKPORT_LINUX_IOSYS_MAP_H

#ifdef BPM_IOSYS_MAP_PRESENT
#include_next <linux/iosys-map.h>
#else
#include <linux/dma-buf-map.h>
#endif

#ifdef BPM_IOSYS_MAP_MEMCPY_TO_ARG_OFFSET_ADDED
#define iosys_map_memcpy_to LINUX_I915_BACKPORT(iosys_map_memcpy_to)
#define iosys_map dma_buf_map

/**
 * iosys_map_memcpy_to - Memcpy into offset of iosys_map
 * @dst:        The iosys_map structure
 * @dst_offset: The offset from which to copy
 * @src:        The source buffer
 * @len:        The number of byte in src
 *
 * Copies data into a iosys_map with an offset. The source buffer is in
 * system memory. Depending on the buffer's location, the helper picks the
 * correct method of accessing the memory.
 */
static inline void iosys_map_memcpy_to(struct iosys_map *dst, size_t dst_offset,
                                       const void *src, size_t len)
{
        if (dst->is_iomem)
                memcpy_toio(dst->vaddr_iomem + dst_offset, src, len);
        else
                memcpy(dst->vaddr + dst_offset, src, len);

}
#endif

#ifdef BPM_IOSYS_MAP_FEW_MORE_HELPER_APIS
/**
 * iosys_map_memcpy_from - Memcpy from iosys_map into system memory
 * @dst:        Destination in system memory
 * @src:        The iosys_map structure
 * @src_offset: The offset from which to copy
 * @len:        The number of byte in src
 *
 * Copies data from a iosys_map with an offset. The dest buffer is in
 * system memory. Depending on the mapping location, the helper picks the
 * correct method of accessing the memory.
 */
static inline void iosys_map_memcpy_from(void *dst, const struct iosys_map *src,
                                         size_t src_offset, size_t len)
{
        if (src->is_iomem)
                memcpy_fromio(dst, src->vaddr_iomem + src_offset, len);
        else
                memcpy(dst, src->vaddr + src_offset, len);
}

/**
 * iosys_map_memset - Memset iosys_map
 * @dst:        The iosys_map structure
 * @offset:     Offset from dst where to start setting value
 * @value:      The value to set
 * @len:        The number of bytes to set in dst
 *
 * Set value in iosys_map. Depending on the buffer's location, the helper
 * picks the correct method of accessing the memory.
 */
static inline void iosys_map_memset(struct iosys_map *dst, size_t offset,
                                    int value, size_t len)
{
        if (dst->is_iomem)
                memset_io(dst->vaddr_iomem + offset, value, len);
        else
                memset(dst->vaddr + offset, value, len);
}

#ifdef CONFIG_64BIT
#define __iosys_map_rd_io_u64_case(val_, vaddr_iomem_)                          \
        u64: val_ = readq(vaddr_iomem_)
#define __iosys_map_wr_io_u64_case(val_, vaddr_iomem_)                          \
        u64: writeq(val_, vaddr_iomem_)
#else
#define __iosys_map_rd_io_u64_case(val_, vaddr_iomem_)                          \
        u64: memcpy_fromio(&(val_), vaddr_iomem_, sizeof(u64))
#define __iosys_map_wr_io_u64_case(val_, vaddr_iomem_)                          \
        u64: memcpy_toio(vaddr_iomem_, &(val_), sizeof(u64))
#endif

#define __iosys_map_rd_io(val__, vaddr_iomem__, type__) _Generic(val__,         \
        u8: val__ = readb(vaddr_iomem__),                                       \
        u16: val__ = readw(vaddr_iomem__),                                      \
        u32: val__ = readl(vaddr_iomem__),                                      \
        __iosys_map_rd_io_u64_case(val__, vaddr_iomem__))

#define __iosys_map_rd_sys(val__, vaddr__, type__)                              \
        val__ = READ_ONCE(*(type__ *)(vaddr__))

#define __iosys_map_wr_io(val__, vaddr_iomem__, type__) _Generic(val__,         \
        u8: writeb(val__, vaddr_iomem__),                                       \
        u16: writew(val__, vaddr_iomem__),                                      \
        u32: writel(val__, vaddr_iomem__),                                      \
        __iosys_map_wr_io_u64_case(val__, vaddr_iomem__))

#define __iosys_map_wr_sys(val__, vaddr__, type__)                              \
        WRITE_ONCE(*(type__ *)(vaddr__), val__)

/**
 * iosys_map_rd - Read a C-type value from the iosys_map
 *
 * @map__:      The iosys_map structure
 * @offset__:   The offset from which to read
 * @type__:     Type of the value being read
 *
 * Read a C type value (u8, u16, u32 and u64) from iosys_map. For other types or
 * if pointer may be unaligned (and problematic for the architecture supported),
 * use iosys_map_memcpy_from().
 *
 * Returns:
 * The value read from the mapping.
 */
#define iosys_map_rd(map__, offset__, type__) ({                                \
        type__ val;                                                             \
        if ((map__)->is_iomem) {                                                \
                __iosys_map_rd_io(val, (map__)->vaddr_iomem + (offset__), type__);\
        } else {                                                                \
                __iosys_map_rd_sys(val, (map__)->vaddr + (offset__), type__);   \
        }                                                                       \
        val;                                                                    \
})

/**
 * iosys_map_wr - Write a C-type value to the iosys_map
 *
 * @map__:      The iosys_map structure
 * @offset__:   The offset from the mapping to write to
 * @type__:     Type of the value being written
 * @val__:      Value to write
 *
 * Write a C type value (u8, u16, u32 and u64) to the iosys_map. For other types
 * or if pointer may be unaligned (and problematic for the architecture
 * supported), use iosys_map_memcpy_to()
 */
#define iosys_map_wr(map__, offset__, type__, val__) ({                         \
        type__ val = (val__);                                                   \
        if ((map__)->is_iomem) {                                                \
                __iosys_map_wr_io(val, (map__)->vaddr_iomem + (offset__), type__);\
        } else {                                                                \
                __iosys_map_wr_sys(val, (map__)->vaddr + (offset__), type__);   \
        }                                                                       \
})

/**
 * IOSYS_MAP_INIT_OFFSET - Initializes struct iosys_map from another iosys_map
 * @map_:       The dma-buf mapping structure to copy from
 * @offset_:    Offset to add to the other mapping
 *
 * Initializes a new iosys_map struct based on another passed as argument. It
 * does a shallow copy of the struct so it's possible to update the back storage
 * without changing where the original map points to. It is the equivalent of
 * doing:
 *
 * .. code-block:: c
 *
 *      iosys_map map = other_map;
 *      iosys_map_incr(&map, &offset);
 *
 * Example usage:
 *
 * .. code-block:: c
 *
 *      void foo(struct device *dev, struct iosys_map *base_map)
 *      {
 *              ...
 *              struct iosys_map map = IOSYS_MAP_INIT_OFFSET(base_map, FIELD_OFFSET);
 *              ...
 *      }
 *
 * The advantage of using the initializer over just increasing the offset with
 * iosys_map_incr() like above is that the new map will always point to the
 * right place of the buffer during its scope. It reduces the risk of updating
 * the wrong part of the buffer and having no compiler warning about that. If
 * the assignment to IOSYS_MAP_INIT_OFFSET() is forgotten, the compiler can warn
 * about the use of uninitialized variable.
 */
#define IOSYS_MAP_INIT_OFFSET(map_, offset_) ({                         \
        struct iosys_map copy = *map_;                                  \
        iosys_map_incr(&copy, offset_);                                 \
        copy;                                                           \
})


/**
 * iosys_map_rd_field - Read a member from a struct in the iosys_map
 *
 * @map__:              The iosys_map structure
 * @struct_offset__:    Offset from the beggining of the map, where the struct
 *                      is located
 * @struct_type__:      The struct describing the layout of the mapping
 * @field__:            Member of the struct to read
 *
 * Read a value from iosys_map considering its layout is described by a C struct
 * starting at @struct_offset__. The field offset and size is calculated and its
 * value read. If the field access would incur in un-aligned access, then either
 * iosys_map_memcpy_from() needs to be used or the architecture must support it.
 * For example: suppose there is a @struct foo defined as below and the value
 * ``foo.field2.inner2`` needs to be read from the iosys_map:
 *
 * .. code-block:: c
 *
 *      struct foo {
 *              int field1;
 *              struct {
 *                      int inner1;
 *                      int inner2;
 *              } field2;
 *              int field3;
 *      } __packed;
 *
 * This is the expected memory layout of a buffer using iosys_map_rd_field():
 *
 * +------------------------------+--------------------------+
 * | Address                      | Content                  |
 * +==============================+==========================+
 * | buffer + 0000                | start of mmapped buffer  |
 * |                              | pointed by iosys_map     |
 * +------------------------------+--------------------------+
 * | ...                          | ...                      |
 * +------------------------------+--------------------------+
 * | buffer + ``struct_offset__`` | start of ``struct foo``  |
 * +------------------------------+--------------------------+
 * | ...                          | ...                      |
 * +------------------------------+--------------------------+
 * | buffer + wwww                | ``foo.field2.inner2``    |
 * +------------------------------+--------------------------+
 * | ...                          | ...                      |
 * +------------------------------+--------------------------+
 * | buffer + yyyy                | end of ``struct foo``    |
 * +------------------------------+--------------------------+
 * | ...                          | ...                      |
 * +------------------------------+--------------------------+
 * | buffer + zzzz                | end of mmaped buffer     |
 * +------------------------------+--------------------------+
 *
 * Values automatically calculated by this macro or not needed are denoted by
 * wwww, yyyy and zzzz. This is the code to read that value:
 *
 * .. code-block:: c
 *
 *      x = iosys_map_rd_field(&map, offset, struct foo, field2.inner2);
 *
 * Returns:
 * The value read from the mapping.
 */
#define iosys_map_rd_field(map__, struct_offset__, struct_type__, field__) ({   \
        struct_type__ *s;                                                       \
        iosys_map_rd(map__, struct_offset__ + offsetof(struct_type__, field__), \
                     typeof(s->field__));                                       \
})

/**
 * iosys_map_wr_field - Write to a member of a struct in the iosys_map
 *
 * @map__:              The iosys_map structure
 * @struct_offset__:    Offset from the beggining of the map, where the struct
 *                      is located
 * @struct_type__:      The struct describing the layout of the mapping
 * @field__:            Member of the struct to read
 * @val__:              Value to write
 *
 * Write a value to the iosys_map considering its layout is described by a C
 * struct starting at @struct_offset__. The field offset and size is calculated
 * and the @val__ is written. If the field access would incur in un-aligned
 * access, then either iosys_map_memcpy_to() needs to be used or the
 * architecture must support it. Refer to iosys_map_rd_field() for expected
 * usage and memory layout.
 */
#define iosys_map_wr_field(map__, struct_offset__, struct_type__, field__, val__) ({    \
        struct_type__ *s;                                                               \
        iosys_map_wr(map__, struct_offset__ + offsetof(struct_type__, field__),         \
                     typeof(s->field__), val__);                                        \
})
#endif /* BPM_IOSYS_MAP_FEW_MORE_HELPER_APIS */

#ifdef BPM_IOSYS_MAP_RENAME_APIS

#define IOSYS_MAP_INIT_VADDR DMA_BUF_MAP_INIT_VADDR
#define iosys_map_set_vaddr dma_buf_map_set_vaddr
#define iosys_map_set_vaddr_iomem dma_buf_map_set_vaddr_iomem
#define iosys_map_is_equal dma_buf_map_is_equal
#define iosys_map_is_null dma_buf_map_is_null
#define iosys_map_is_set dma_buf_map_is_set
#define iosys_map_clear dma_buf_map_clear
#define iosys_map_incr dma_buf_map_incr
#endif

#endif /* __BACKPORT_LINUX_IOSYS_MAP_H */
