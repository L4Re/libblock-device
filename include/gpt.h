/*
 * Copyright (C) 2018, 2024 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <l4/sys/types.h>

namespace Block_device {
namespace Gpt {

struct Header
{
  char         signature[8];
  l4_uint32_t  version;
  l4_uint32_t  header_size;
  l4_uint32_t  crc;
  l4_uint32_t  _reserved;
  l4_uint64_t  current_lba;
  l4_uint64_t  backup_lba;
  l4_uint64_t  first_lba;
  l4_uint64_t  last_lba;
  char         disk_guid[16];
  l4_uint64_t  partition_array_lba;
  l4_uint32_t  partition_array_size;
  l4_uint32_t  entry_size;
  l4_uint32_t  crc_array;
} __attribute__((packed));

struct Entry
{
  unsigned char type_guid[16];
  unsigned char partition_guid[16];
  l4_uint64_t   first;
  l4_uint64_t   last;
  l4_uint64_t   flags;
  l4_uint16_t   name[36];
};

} // namespace
} // namespace
