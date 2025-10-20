/*
 * Copyright (C) 2025 Kernkonzept GmbH.
 * Author(s): Jakub Jermar <jakub.jermar@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/sys/types.h>

namespace Block_device {
namespace Mbr {

enum
{
  Primary_partitions = 4
};

enum
{
  Magic_lo = 0x55,
  Magic_hi = 0xaa
};

enum Partition_type
{
  Extended = 0x5,
  Linux = 0x83
};

struct Entry
{
  l4_uint8_t   attr;
  l4_uint8_t   chs_start[3];
  l4_uint8_t   type;
  l4_uint8_t   chs_last[3];
  l4_uint32_t  lba_start;
  l4_uint32_t  lba_num;
} __attribute__((packed));

struct Mbr
{
  l4_uint8_t   code[440];
  l4_uint32_t  disk_id;
  l4_uint16_t  reserved;
  Entry        partition[4];
  l4_uint8_t   signature[2];
} __attribute__((packed));

} // namespace

namespace Ext2 {

enum
{
  Superblock_offset = 1024,
  Superblock_magic = 0xef53,
  Superblock_version = 1,
};

// Ext2 superblock with fields essential for getting the "label".
struct Superblock
{
  l4_uint8_t ignore[56];
  l4_uint16_t magic;
  l4_uint8_t ignore2[18];
  l4_uint32_t major;
  l4_uint8_t ignore3[24];
  l4_uint8_t fs_id[16];
  l4_uint8_t name[16];
} __attribute__((packed));

} // namespace

} // namespace
