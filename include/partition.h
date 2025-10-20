/*
 * Copyright (C) 2018, 2020-2025 Kernkonzept GmbH.
 * Author(s): Sarah Hoffmann <sarah.hoffmann@kernkonzept.com>
 *            Jakub Jermar <jakub.jermar@kernkonzept.com>
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
#include <l4/libblock-device/mbr.h>

#include <l4/sys/cache.h>

namespace Block_device {

/**
 * Information about a single partition.
 */
struct Partition_info
{
  char           uuid[37];  ///< ID of the partition.
  std::u16string name;      ///< UTF16 name of the partition.
  l4_uint64_t    first;     ///< First valid sector.
  l4_uint64_t    last;      ///< Last valid sector.
  l4_uint64_t    flags;     ///< Additional flags, depending on partition type.
};

/**
 * Interface of a partition table reader for block devices.
 */
class Reader : public cxx::Ref_obj
{
public:
  virtual ~Reader() = default;

  virtual void read(Errand::Callback const &callback) = 0;
  virtual l4_size_t table_size() const = 0;
  virtual int get_partition(l4_size_t idx, Partition_info *inf) const = 0;
};

/**
 * Class providing common functionality for partition table readers.
 */
template <typename DERIVED, typename DEV>
class Base_reader : public Reader
{
protected:
  enum
  {
    Max_partitions = 1024  ///< Maximum number of partitions to be scanned.
  };

public:
  using Device_type = DEV;

  Base_reader(Device_type *dev)
  : _dev(dev)
  {}

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

  Inout_block _db;
  Device_type *_dev;
  Errand::Callback _callback;
};

/**
 * GPT partition table reader.
 */
template <typename DEV>
class Gpt_reader : public Base_reader<Gpt_reader<DEV>, DEV>
{
  using Device_type = DEV;
  using Base = Base_reader<Gpt_reader<DEV>, Device_type>;

public:
  Gpt_reader(Device_type *dev)
  : Base_reader<Gpt_reader<Device_type>, Device_type>(dev),
    _header(2, dev, L4Re::Dma_space::Direction::From_device)
  {}

  l4_size_t table_size() const override
  { return _num_partitions; }

  void read(Errand::Callback const &callback) override
  {
    _num_partitions = 0;
    Base::_callback = callback;

    // preparation: read the first two sectors
    Base::_db = _header.inout_block();
    Base::read_sectors(0, &Gpt_reader<Device_type>::get_gpt);
  }

  int get_partition(l4_size_t idx, Partition_info *inf) const override
  {
    if (idx == 0 || idx > _num_partitions)
      return -L4_ERANGE;

    unsigned secsz = Base::_dev->sector_size();
    auto *header = _header.template get<Gpt::Header const>(secsz);

    Gpt::Entry *e =
      _parray.template get<Gpt::Entry>((idx - 1) * header->entry_size);

    if (*((l4_uint64_t *) &e->partition_guid) == 0ULL)
      return -L4_ENODEV;

    render_guid(e->partition_guid, inf->uuid);

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
                    inf->uuid);

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
    _header.unmap();

    if (error < 0)
      {
        // can't read from device, we are done
        Base::invoke_callback();
        return;
      }

    // prepare reading of the table from disk
    unsigned secsz = Base::_dev->sector_size();
    auto *header = _header.template get<Gpt::Header const>(secsz);

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

    _num_partitions =
      cxx::min<l4_uint32_t>(header->partition_array_size, Base::Max_partitions);

    l4_size_t arraysz = _num_partitions * header->entry_size;
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
      _num_partitions = 0;

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

  l4_size_t _num_partitions;
  Inout_memory<Device_type> _header;
  Inout_memory<Device_type> _parray;
};

/**
 * MBR partition table reader.
 *
 * Handles primary and logical partitions.
 */
template <typename DEV>
class Mbr_reader : public Base_reader<Mbr_reader<DEV>, DEV>
{
  using Device_type = DEV;
  using Base = Base_reader<Mbr_reader<DEV>, Device_type>;

  enum : unsigned
  {
    // Limit the number of MBR partitions to 255 so that they all can have
    // a unique UUID.
    Max_partitions = cxx::min<unsigned>(Base::Max_partitions, 255u)
  };

public:
  Mbr_reader(Device_type *dev)
  : Base_reader<Mbr_reader<Device_type>, Device_type>(dev),
    _mbr(nullptr),
    _header(1, dev, L4Re::Dma_space::Direction::From_device),
    _partitions(Mbr::Primary_partitions),
    _extended(nullptr),
    _lba_offset_ext(0)
  {}

  l4_size_t table_size() const override
  {
    return _num_partitions;
  }

  void read(Errand::Callback const &callback) override
  {
    _num_partitions = 0;
    Base::_callback = callback;

    // preparation: read the first sector
    Base::_db = _header.inout_block();
    Base::read_sectors(0, &Mbr_reader<DEV>::get_mbr);
  }

  int get_partition(l4_size_t idx, Partition_info *inf) const override
  {
    if (idx == 0 || idx > _num_partitions)
      return -L4_ERANGE;

    *inf = _partitions[idx - 1];

    if (inf->first > inf->last)
      return -L4_ENODEV;

    return L4_EOK;
  }

private:
  bool register_mbr_partition(Mbr::Entry const *p, l4_uint32_t disk_id,
                              unsigned n)
  {
    _partitions.resize(n + 1);

    auto info = Dbg::info();

    bool valid = false;
    if (p->type && p->lba_start <= p->lba_start + p->lba_num - 1)
      {
        valid = true;
        snprintf(_partitions[n].uuid, sizeof(_partitions[n].uuid), "%08X-%02X",
                 disk_id, n + 1);
        _partitions[n].first = _lba_offset_ext + p->lba_start;
        _partitions[n].last = _lba_offset_ext + p->lba_start + p->lba_num - 1;
        _partitions[n].flags = p->type;

        info.printf("Partition %u, UUID=%s: start=%llu size=%zu, type=%x\n",
                    n + 1, _partitions[n].uuid, _partitions[n].first,
                    p->lba_num * Base::_dev->sector_size(), p->type);

        if (p->type == Mbr::Partition_type::Extended && !_extended)
          _extended = p;
      }
    else
      {
        _partitions[n].uuid[0] = 0;
        _partitions[n].first = -1ULL;
        _partitions[n].last = 0ULL;
        _partitions[n].flags = 0ULL;
      }

    return valid;
  }

  void get_mbr(int error, l4_size_t)
  {
    _header.unmap();

    if (error < 0)
      {
        // can't read from device, we are done
        Base::invoke_callback();
        return;
      }

    _mbr = _header.template get<Mbr::Mbr const>(0);

    auto info = Dbg::info();

    if (_mbr->signature[0] != Mbr::Magic_lo
        || _mbr->signature[1] != Mbr::Magic_hi)
      {
        info.printf("No MBR found.\n");
        Base::invoke_callback();
        return;
      }

    info.printf("MBR found.\n");

    for (unsigned i = 0; i < Mbr::Primary_partitions; i++)
      (void)register_mbr_partition(&_mbr->partition[i], _mbr->disk_id, i);

    _num_partitions = Mbr::Primary_partitions;
    if (_extended)
      {
        _ep =
          Inout_memory<Device_type>(1, Base::_dev,
                                    L4Re::Dma_space::Direction::From_device);
        Base::_db = _ep.inout_block();
        _lba_offset_ext += _extended->lba_start;
        Base::read_sectors(_extended->lba_start, &Mbr_reader<DEV>::get_ext);
      }
    else
      Base::invoke_callback();
  }

  void get_ext(int error, l4_size_t)
  {
    _ep.unmap();

    if (error < 0)
      {
        // can't read from device, we are done
        Base::invoke_callback();
        return;
      }

    auto *ext = _ep.template get<Mbr::Mbr const>(0);

    auto info = Dbg::info();
    if (ext->signature[0] != Mbr::Magic_lo
        || ext->signature[1] != Mbr::Magic_hi)
      {
        info.printf("No extended MBR found.\n");
        Base::invoke_callback();
        return;
      }

    info.printf("Extended MBR found at LBA %u.\n", _lba_offset_ext);

    if (register_mbr_partition(&ext->partition[0], _mbr->disk_id,
                               _num_partitions))
      {
        _num_partitions++;
        l4_uint32_t next = ext->partition[1].lba_start;
        if (next && _num_partitions < Max_partitions)
          {
            _ep = Inout_memory<Device_type>(
              1, Base::_dev, L4Re::Dma_space::Direction::From_device);
            Base::_db = _ep.inout_block();
            _lba_offset_ext = _extended->lba_start + next;
            Base::read_sectors(_extended->lba_start + next,
                               &Mbr_reader<Device_type>::get_ext);
            return;
          }
      }
    Base::invoke_callback();
  }

  l4_size_t _num_partitions;
  Mbr::Mbr const *_mbr;
  Inout_memory<Device_type> _header;
  std::vector<Partition_info> _partitions;
  Mbr::Entry const *_extended; // Pointer to the first extended MBR
  Inout_memory<Device_type> _ep;
  l4_uint32_t _lba_offset_ext; // Extended partition offset
};

/// Generic partition reader which tries to read GPT first and MBR second
/// as a fallback.
template <typename DEV>
class Partition_reader : public Base_reader<Partition_reader<DEV>, DEV>
{
  using Device_type = DEV;
  using Base = Base_reader<Partition_reader<DEV>, Device_type>;

public:
  Partition_reader(Device_type *dev)
  : Base_reader<Partition_reader<Device_type>, Device_type>(dev),
    _reader(cxx::make_ref_obj<Gpt_reader<Device_type>>(dev))
  {}

  l4_size_t table_size() const override
  {
    return _reader->table_size();
  }

  void read(Errand::Callback const &callback) override
  {
    Base::_callback = callback; // hold ref to the original callback
    _reader->read([=]()
      {
        // If the GPT reader fails to discover any partitions, we carefully
        // switch to the MBR reader. When the MBR reader's callback is called
        // we are done whether it found any partitions or not. In any case
        // this ends by invoking the original callback.
        if (_reader->table_size() == 0)
          {
            // Hold ref to the old reader as we are still executing its lambda.
            auto old = _reader;
            _reader = cxx::make_ref_obj<Mbr_reader<Device_type>>(Base::_dev);
            _reader->read([=](){ Base::invoke_callback();});
          }
        else
          Base::invoke_callback();
      });
  }

  int get_partition(l4_size_t idx, Partition_info *inf) const override
  {
    return _reader->get_partition(idx, inf);
  }

private:
  cxx::Ref_ptr<Reader> _reader;
};

}
