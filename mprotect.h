/*
 * BRIEF DESCRIPTION
 *
 * Memory protection definitions for the NOVA filesystem.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 *
 * This program is free software; you can redistribute it and/or modify it
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __WPROTECT_H
#define __WPROTECT_H

#include <linux/fs.h>
#include "nova_def.h"

/* nova_memunlock_super() before calling! */
static inline void nova_sync_super(struct nova_super_block *ps)
{
	u16 crc = 0;

	ps->s_wtime = cpu_to_le32(get_seconds());
	ps->s_sum = 0;
	crc = crc16(~0, (__u8 *)ps + sizeof(__le16),
			NOVA_SB_STATIC_SIZE(ps) - sizeof(__le16));
	ps->s_sum = cpu_to_le16(crc);
	/* Keep sync redundant super block */
	memcpy((void *)ps + NOVA_SB_SIZE, (void *)ps,
		sizeof(struct nova_super_block));
}

#if 0
/* nova_memunlock_inode() before calling! */
static inline void nova_sync_inode(struct nova_inode *pi)
{
	u16 crc = 0;

	pi->i_sum = 0;
	crc = crc16(~0, (__u8 *)pi + sizeof(__le16), NOVA_INODE_SIZE -
		    sizeof(__le16));
	pi->i_sum = cpu_to_le16(crc);
}
#endif

extern int nova_writeable(void *vaddr, unsigned long size, int rw);
extern int nova_dax_mem_protect(struct super_block *sb,
				 void *vaddr, unsigned long size, int rw);
int nova_mmap_to_new_blocks(struct vm_area_struct *vma,
	unsigned long address, int num_blocks);
int nova_set_vmas_readonly(struct super_block *sb);

static inline int nova_is_protected(struct super_block *sb)
{
	struct nova_sb_info *sbi = (struct nova_sb_info *)sb->s_fs_info;

	if (wprotect)
		return wprotect;

	return sbi->s_mount_opt & NOVA_MOUNT_PROTECT;
}

static inline int nova_is_wprotected(struct super_block *sb)
{
	return nova_is_protected(sb);
}

static inline void
__nova_memunlock_range(void *p, unsigned long len)
{
	/*
	 * NOTE: Ideally we should lock all the kernel to be memory safe
	 * and avoid to write in the protected memory,
	 * obviously it's not possible, so we only serialize
	 * the operations at fs level. We can't disable the interrupts
	 * because we could have a deadlock in this path.
	 */
	nova_writeable(p, len, 1);
}

static inline void
__nova_memlock_range(void *p, unsigned long len)
{
	nova_writeable(p, len, 0);
}

static inline void nova_memunlock_range(struct super_block *sb, void *p,
					 unsigned long len)
{
	if (nova_is_protected(sb))
		__nova_memunlock_range(p, len);
}

static inline void nova_memlock_range(struct super_block *sb, void *p,
				       unsigned long len)
{
	if (nova_is_protected(sb))
		__nova_memlock_range(p, len);
}

static inline void nova_memunlock_super(struct super_block *sb,
					 struct nova_super_block *ps)
{
	if (nova_is_protected(sb))
		__nova_memunlock_range(ps, NOVA_SB_SIZE);
}

static inline void nova_memlock_super(struct super_block *sb,
				       struct nova_super_block *ps)
{
	nova_sync_super(ps);
	if (nova_is_protected(sb))
		__nova_memlock_range(ps, NOVA_SB_SIZE);
}

static inline void nova_memunlock_reserved(struct super_block *sb,
					 struct nova_super_block *ps)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	if (nova_is_protected(sb))
		__nova_memunlock_range(ps,
			sbi->reserved_blocks * NOVA_DEF_BLOCK_SIZE_4K);
}

static inline void nova_memlock_reserved(struct super_block *sb,
				       struct nova_super_block *ps)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	if (nova_is_protected(sb))
		__nova_memlock_range(ps,
			sbi->reserved_blocks * NOVA_DEF_BLOCK_SIZE_4K);
}

static inline void nova_memunlock_journal(struct super_block *sb)
{
	void *addr = nova_get_block(sb, NOVA_DEF_BLOCK_SIZE_4K * JOURNAL_START);
	if (nova_is_protected(sb))
		__nova_memunlock_range(addr, NOVA_DEF_BLOCK_SIZE_4K);
}

static inline void nova_memlock_journal(struct super_block *sb)
{
	void *addr = nova_get_block(sb, NOVA_DEF_BLOCK_SIZE_4K * JOURNAL_START);
	if (nova_is_protected(sb))
		__nova_memlock_range(addr, NOVA_DEF_BLOCK_SIZE_4K);
}

static inline void nova_memunlock_inode(struct super_block *sb,
					 struct nova_inode *pi)
{
	if (nova_is_protected(sb))
		__nova_memunlock_range(pi, NOVA_INODE_SIZE);
}

static inline void nova_memlock_inode(struct super_block *sb,
				       struct nova_inode *pi)
{
	/* nova_sync_inode(pi); */
	if (nova_is_protected(sb))
		__nova_memlock_range(pi, NOVA_INODE_SIZE);
}

static inline void nova_memunlock_block(struct super_block *sb, void *bp)
{
	if (nova_is_protected(sb))
		__nova_memunlock_range(bp, sb->s_blocksize);
}

static inline void nova_memlock_block(struct super_block *sb, void *bp)
{
	if (nova_is_protected(sb))
		__nova_memlock_range(bp, sb->s_blocksize);
}

#endif
