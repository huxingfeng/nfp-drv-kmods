/*
 * Copyright (C) 2015 Netronome Systems, Inc.
 *
 * This software is dual licensed under the GNU General License Version 2,
 * June 1991 as shown in the file COPYING in the top-level directory of this
 * source tree or the BSD 2-Clause License provided below.  You have the
 * option to license this software under the complete terms of either license.
 *
 * The BSD 2-Clause License:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      1. Redistributions of source code must retain the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer.
 *
 *      2. Redistributions in binary form must reproduce the above
 *         copyright notice, this list of conditions and the following
 *         disclaimer in the documentation and/or other materials
 *         provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * nfp_resource.c
 * Author: Jakub Kicinski <jakub.kicinski@netronome.com>
 *         Jason McMullan <jason.mcmullan@netronome.com>
 */
#include <linux/kernel.h>
#include <linux/slab.h>

#include "crc32.h"
#include "nfp.h"
#include "nfp_cpp.h"
#include "nfp6000/nfp6000.h"

#define NFP_RESOURCE_ENTRY_NAME_SZ	8

/**
 * struct nfp_resource_entry - Resource table entry
 * @owner:		NFP CPP Lock, interface owner
 * @key:		NFP CPP Lock, posix_crc32(name, 8)
 * @region:		Memory region descriptor
 * @name:		ASCII, zero padded name
 * @reserved
 * @cpp_action:		CPP Action
 * @cpp_token:		CPP Token
 * @cpp_target:		CPP Target ID
 * @page_offset:	256-byte page offset into target's CPP address
 * @page_size:		size, in 256-byte pages
 */
struct nfp_resource_entry {
	struct nfp_resource_entry_mutex {
		u32 owner;
		u32 key;
	} mutex;
	struct nfp_resource_entry_region {
		u8  name[NFP_RESOURCE_ENTRY_NAME_SZ];
		u8  reserved[5];
		u8  cpp_action;
		u8  cpp_token;
		u8  cpp_target;
		u32 page_offset;
		u32 page_size;
	} region;
};

#define NFP_RESOURCE_TBL_SIZE		4096
#define NFP_RESOURCE_TBL_ENTRIES	(NFP_RESOURCE_TBL_SIZE /	\
					 sizeof(struct nfp_resource_entry))

struct nfp_resource {
	char name[NFP_RESOURCE_ENTRY_NAME_SZ + 1];
	u32 cpp_id;
	u64 addr;
	u64 size;
	struct nfp_cpp_mutex *mutex;
};

static int nfp_cpp_resource_acquire(struct nfp_cpp *cpp, const char *name,
				    u32 *r_cpp, u64 *r_addr, u64 *r_size,
				    struct nfp_cpp_mutex **resource_mutex)
{
	char name_pad[NFP_RESOURCE_ENTRY_NAME_SZ] = {};
	struct nfp_resource_entry entry;
	struct nfp_cpp_mutex *mutex;
	u32 cpp_id, key;
	int err, i;

	cpp_id = NFP_CPP_ID(NFP_RESOURCE_TBL_TARGET, 3, 0);  /* Atomic read */

	mutex = nfp_device_lock(cpp);
	if (!mutex)
		return -ENOMEM;

	strncpy(name_pad, name, sizeof(name_pad));

	/* Search for a matching entry */
	key = NFP_RESOURCE_TBL_KEY;
	if (memcmp(name_pad, NFP_RESOURCE_TBL_NAME "\0\0\0\0\0\0\0\0", 8))
		key = crc32_posix(name_pad, sizeof(name_pad));

	err = -ENOENT;
	for (i = 0; i < NFP_RESOURCE_TBL_ENTRIES; i++) {
		u64 addr = NFP_RESOURCE_TBL_BASE +
			sizeof(struct nfp_resource_entry) * i;

		err = nfp_cpp_read(cpp, cpp_id, addr, &entry, sizeof(entry));
		if (err != sizeof(entry)) {
			err = -EIO;
			goto exit_unlock;
		}

		if (entry.mutex.key != key)
			continue;

		/* Found key! */
		*resource_mutex =
			nfp_cpp_mutex_alloc(cpp,
					    NFP_RESOURCE_TBL_TARGET, addr, key);
		*r_cpp = NFP_CPP_ID(entry.region.cpp_target,
				    entry.region.cpp_action,
				    entry.region.cpp_token);
		*r_addr = (u64)entry.region.page_offset << 8;
		*r_size = (u64)entry.region.page_size << 8;

		err = 0;
		break;
	}

exit_unlock:
	nfp_device_unlock(cpp, mutex);

	return err;
}

/**
 * nfp_resource_acquire() - Acquire a resource handle
 * @cpp:	NFP CPP handle
 * @name:	Name of the resource
 *
 * NOTE: This function locks the acquired resource
 *
 * Return: NFP Resource handle, or ERR_PTR()
 */
struct nfp_resource *
nfp_resource_acquire(struct nfp_cpp *cpp, const char *name)
{
	struct nfp_cpp_mutex *mutex;
	struct nfp_resource *res;
	u64 addr, size;
	u32 cpp_id;
	int err;

	err = nfp_cpp_resource_acquire(cpp, name, &cpp_id, &addr,
				       &size, &mutex);
	if (err)
		return ERR_PTR(err);

	err = nfp_cpp_mutex_lock(mutex);
	if (err) {
		nfp_cpp_mutex_free(mutex);
		return ERR_PTR(err);
	}

	res = kzalloc(sizeof(*res), GFP_KERNEL);
	if (!res) {
		nfp_cpp_mutex_unlock(mutex);
		nfp_cpp_mutex_free(mutex);
		return ERR_PTR(-ENOMEM);
	}

	strncpy(res->name, name, NFP_RESOURCE_ENTRY_NAME_SZ);
	res->cpp_id = cpp_id;
	res->addr = addr;
	res->size = size;
	res->mutex = mutex;

	return res;
}

/**
 * nfp_resource_release() - Release a NFP Resource handle
 * @res:	NFP Resource handle
 *
 * NOTE: This function implictly unlocks the resource handle
 */
void nfp_resource_release(struct nfp_resource *res)
{
	nfp_cpp_mutex_unlock(res->mutex);
	nfp_cpp_mutex_free(res->mutex);
	kfree(res);
}

/**
 * nfp_resource_cpp_id() - Return the cpp_id of a resource handle
 * @res:	NFP Resource handle
 *
 * Return: NFP CPP ID
 */
u32 nfp_resource_cpp_id(struct nfp_resource *res)
{
	return res->cpp_id;
}

/**
 * nfp_resource_name() - Return the name of a resource handle
 * @res:	NFP Resource handle
 *
 * Return: const char pointer to the name of the resource
 */
const char *nfp_resource_name(struct nfp_resource *res)
{
	return res->name;
}

/**
 * nfp_resource_address() - Return the address of a resource handle
 * @res:	NFP Resource handle
 *
 * Return: Address of the resource
 */
u64 nfp_resource_address(struct nfp_resource *res)
{
	return res->addr;
}

/**
 * nfp_resource_size() - Return the size in bytes of a resource handle
 * @res:	NFP Resource handle
 *
 * Return: Size of the resource in bytes
 */
u64 nfp_resource_size(struct nfp_resource *res)
{
	return res->size;
}
