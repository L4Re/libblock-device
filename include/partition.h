/*
 * Copyright (C) 2018, 2020-2024 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
#pragma once

#include <cstring>
#include <string>
#include <cassert>

#include <l4/cxx/ref_ptr>

#include <l4/l4virtio/virtio_block.h>

#include <l4/libblock-device/debug.h>
#include <l4/libblock-device/errand.h>
#include <l4/libblock-device/inout_memory.h>
#include <l4/libblock-device/gpt.h>

#include <l4/sys/cache.h>

namespace Block_device {

/**
 * Information about a single partition.
 */
struct Partition_info
{
  char           guid[37];  ///< ID of the partition.
  std::u16string name;      ///< UTF16 name of the partition.
  l4_uint64_t    first;     ///< First valid sector.
  l4_uint64_t    last;      ///< Last valid sector.
  l4_uint64_t    flags;     ///< Additional flags, depending on partition type.
};


/**
 * Partition table reader for block devices.
 */
template <typename DERIVED, typename DEV>
class Partition_reader : public cxx::Ref_obj
{
protected:
  enum
  {
    Max_partitions = 1024  ///< Maximum number of partitions to be scanned.
  };

public:
  using Device_type = DEV;

  Partition_reader(Device_type *dev, l4_uint32_t hdr_sectors)
  : _num_partitions(0),
    _dev(dev),
    _header(hdr_sectors, dev, L4Re::Dma_space::Direction::From_device)
  {}

  virtual ~Partition_reader() = default;

  virtual void read(Errand::Callback const &callback) = 0;

  l4_size_t table_size() const
  { return _num_partitions; }

  virtual int get_partition(l4_size_t idx, Partition_info *inf) const = 0;

protected:
  void invoke_callback()
  {
    assert(_callback);
    _callback();
    // Reset the callback to drop potential transitive self-references
    _callback = nullptr;
  }

  void read_sectors(l4_uint64_t sector,
                    void (DERIVED::*func)(int, l4_size_t))
  {
    using namespace std::placeholders;
    auto next = std::bind(func, static_cast<DERIVED *>(this), _1, _2);

    l4_addr_t vstart = reinterpret_cast<l4_addr_t>(_db.virt_addr);
    l4_addr_t vend   = vstart + _db.num_sectors * _dev->sector_size();
    l4_cache_inv_data(vstart, vend);

    Errand::poll(10, 10000,
                 [=]()
                   {
                     int ret = _dev->inout_data(
                                 sector, _db,
                                 [next, vstart, vend](int error, l4_size_t size)
                                   {
                                     l4_cache_inv_data(vstart, vend);
                                     next(error, size);
                                   },
                                   L4Re::Dma_space::Direction::From_device);
                     if (ret < 0 && ret != -L4_EBUSY)
                       invoke_callback();
                     return ret != -L4_EBUSY;
                   },
                 [=](bool ret) { if (!ret) invoke_callback(); }
                );
  }

  l4_size_t _num_partitions;
  Inout_block _db;
  Device_type *_dev;
  Inout_memory<Device_type> _header;
  Errand::Callback _callback;
};

template <typename DEV>
class Gpt_reader : public Partition_reader<Gpt_reader<DEV>, DEV>
{
  using Device_type = DEV;
  using Base = Partition_reader<Gpt_reader<DEV>, Device_type>;

public:
  Gpt_reader(Device_type *dev)
  : Partition_reader<Gpt_reader<Device_type>, Device_type>(dev, 2)
  {}

  void read(Errand::Callback const &callback) override
  {
    Base::_num_partitions = 0;
    Base::_callback = callback;

    // preparation: read the first two sectors
    Base::_db = Base::_header.inout_block();
    Base::read_sectors(0, &Gpt_reader<Device_type>::get_gpt);
  }

  int get_partition(l4_size_t idx, Partition_info *inf) const override
  {
    if (idx == 0 || idx > Base::_num_partitions)
      return -L4_ERANGE;

    unsigned secsz = Base::_dev->sector_size();
    auto *header = Base::_header.template get<Gpt::Header const>(secsz);

    Gpt::Entry *e =
      _parray.template get<Gpt::Entry>((idx - 1) * header->entry_size);

    if (*((l4_uint64_t *) &e->partition_guid) == 0ULL)
      return -L4_ENODEV;

    render_guid(e->partition_guid, inf->guid);

    auto name =
      std::u16string((char16_t *)e->name, sizeof(e->name) / sizeof(e->name[0]));
    inf->name = name.substr(0, name.find((char16_t) 0));

    inf->first = e->first;
    inf->last = e->last;
    inf->flags = e->flags;

    auto info = Dbg::info();
    if (info.is_active())
      {
        info.printf("%3zu: %10lld %10lld  %5gMiB [%.37s]\n",
                    idx, e->first, e->last,
                    (e->last - e->first + 1.0) * secsz / (1 << 20),
                    inf->guid);

        char buf[37];
        info.printf("   : Type: %s\n", render_guid(e->type_guid, buf));
      }

    auto warn = Dbg::warn();
    if (inf->last < inf->first)
      {
        warn.printf(
          "Invalid settings of %3zu. Last lba before first lba. Will ignore.\n",
          idx);
        // Errors in the GPT shall not crash any service -- just ignore the
        // corresponding partition.
        return -L4_ENODEV;
      }

    return L4_EOK;
  }

private:
  void get_gpt(int error, l4_size_t)
  {
    Base::_header.unmap();

    if (error < 0)
      {
        // can't read from device, we are done
        Base::invoke_callback();
        return;
      }

    // prepare reading of the table from disk
    unsigned secsz = Base::_dev->sector_size();
    auto *header = Base::_header.template get<Gpt::Header const>(secsz);

    auto info = Dbg::info();
    auto trace = Dbg::trace();

    if (strncmp(header->signature, "EFI PART", 8) != 0)
      {
        info.printf("No GUID partition header found.\n");
        Base::invoke_callback();
        return;
      }

    // XXX check CRC32 of header

    info.printf("GUID partition header found with up to %d partitions.\n",
                header->partition_array_size);
    char buf[37];
    info.printf("GUID: %s\n", render_guid(header->disk_guid, buf));
    trace.printf("Header positions: %llx (Backup: %llx)\n",
                 header->current_lba, header->backup_lba);
    trace.printf("First + last: %llx and %llx\n",
                 header->first_lba, header->last_lba);
    trace.printf("Partition table at %llx\n",
                 header->partition_array_lba);
    trace.printf("Size of a partition entry: %d\n",
                 header->entry_size);

    info.printf("GUID partition header found with %d partitions.\n",
                header->partition_array_size);

    Base::_num_partitions =
      cxx::min<l4_uint32_t>(header->partition_array_size, Base::Max_partitions);

    l4_size_t arraysz = Base::_num_partitions * header->entry_size;
    l4_size_t numsec = (arraysz - 1 + secsz) / secsz;

    _parray =
      Inout_memory<Device_type>(numsec, Base::_dev,
                                L4Re::Dma_space::Direction::From_device);
    trace.printf("Reading GPT table @ 0x%p\n", _parray.template get<void>(0));

    Base::_db = _parray.inout_block();
    Base::read_sectors(header->partition_array_lba, &Gpt_reader<DEV>::done_gpt);
  }

  void done_gpt(int error, l4_size_t)
  {
    _parray.unmap();

    // XXX check CRC32 of table

    if (error < 0)
      Base::_num_partitions = 0;

    Base::invoke_callback();
  }

  static char const *render_guid(void const *guid_p, char buf[])
  {
    auto *p = static_cast<unsigned char const *>(guid_p);
    snprintf(buf, 37,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             p[3],  p[2], p[1],  p[0], p[5],  p[4], p[7],  p[6],
             p[8],  p[9], p[10], p[11], p[12], p[13], p[14], p[15]);

    return buf;
  }

  Inout_memory<Device_type> _parray;
};

}
