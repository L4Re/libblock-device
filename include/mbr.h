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
  Extended = 0x5
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
} // namespace
