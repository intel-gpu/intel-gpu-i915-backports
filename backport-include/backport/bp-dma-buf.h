#ifndef __BACKPORT_DMA_BUF_IN_H
#define __BACKPORT_DMA_BUF_IN_H

#define sync_debugfs_init LINUX_DMABUF_BACKPORT(sync_debugfs_init)
int __init sync_debugfs_init(void);
#endif // __BACKPORT_DMA_BUF_IN_H
