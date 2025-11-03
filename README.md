# L4Re Block Device Library

libblock-device is a library used by block device servers. It takes
care of processing requests from L4virtio block clients, queuing them and
dispatching them to the associated device.  It also scans devices for
partitions and pairs clients with the found disks and partitions.

The library provides an abstraction for a block device. Block device
implementations must be provided by the user of the library, but the library
provides an implementation for partitioned devices on top of user block
device implementations.

# Documentation

This package is part of the L4Re operating system. For documentation and
build instructions please refer to [l4re.org](https://l4re.org).

# Contributions

We welcome contributions. Please see the
[contributors guide](https://l4re.org/contributing/).

# License

Detailed licensing and copyright information can be found in the
[LICENSE](LICENSE.spdx) file.
