.. SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
..
.. SPDX-License-Identifier: BSD-3-Clause

=============================
 xNVMe third-party directory
=============================

Take a look at ``.reuse/dep5`` as well and produce a SBOM via the
REUSE-software tool::

  reuse spdx

This file and directory contain information about the software on which xNVMe
depend. The licenses and links to project web-pages and sources are provided
and a brief summary of how xNVMe uses the third-party software.

REPOS/subprojects/spdk/
  Summary: For the xNVMe backend using SPDK, then this git submodule is
  provided and tagged with the SPDK version on which xNVMe has been tested.

  License: BSD-3-Clause
  https://github.com/spdk/spdk/blob/master/LICENSE

  Repos: https://github.com/spdk/spdk
  Website: https://spdk.io/

REPOS/toolbox/xnvme-driver.sh
  Summary: This script is based on the setup.sh and its associated scripts from
  SPDK. It is added here in a single-file for easy distribution with xNVMe.

  License: BSD-3-Clause
  https://github.com/spdk/spdk/blob/master/LICENSE

  Repos: https://github.com/spdk/spdk
  Website: https://spdk.io/

REPOS/toolbox/third-party/linux/upcie/
  Summary: Header-only library providing the user-space PCIe, IOMMU and NVMe
  primitives that the ``be/upcie`` backend is built on, along with the CUDA
  and HIP device-memory heaps its GPU variants allocate from. It is vendored
  as a plain copy of a release, not a submodule, and refreshed wholesale by
  commits titled ``chore(upcie): vendor uPCIe vX.Y.Z``.

  License: BSD-3-Clause

  Repos: https://github.com/safl/upcie
  Website: https://github.com/safl/upcie

  Local changes: the copy in this tree is NOT pristine. The freelists in
  ``dmamem_heap.h``, ``cudamem_heap.h`` and ``hipmem_heap.h`` carry a
  per-heap mutex that upstream does not have, because xNVMe allocates from
  several threads at once. A refresh overwrites the directory and drops
  those. Before refreshing, capture the delta against the commit that
  vendored the current release::

    git log --oneline -- toolbox/third-party/linux/upcie/   # find the last "vendor uPCIe" commit
    git diff <that-commit> -- toolbox/third-party/linux/upcie/ > upcie-local.patch

  then drop in the new release and re-apply. Sending the change upstream is
  the better fix, since then the next refresh carries it for free.

SYSTEM:freebsd:/
  Summary: Headers for the FreeBSD Kernel NVMe driver IOCTLs are used by the
  ``be/fbsd``, then headers for backend. These headers are not distributed with
  xNVMe but rather expected to be available on the system where xNVMe is built.

  License: BSD-3-Clause

  Repos: https://svnweb.freebsd.org/
  Website: https://www.freebsd.org/

SYSTEM::linux:/
  Summary: Headers for the Linux Kernel NVMe driver IOCTLs are used by the
  ``be/linux`` backend. These headers are not distributed with xNVMe but rather
  expected to be available on the system where xNVMe is built.

  License: GPL-2.0 WITH Linux-syscall-note
  https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/nvme_ioctl.h?h=v5.3-rc8

  Repos: https://github.com/spdk/spdk
  Website: https://www.linuxfoundation.org/
