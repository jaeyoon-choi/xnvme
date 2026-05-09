.. footer::

   ###Page### / ###Total###

.. header::

   ###Title### - ###Section###

====================================
 {{title}} - {{subtitle}}
====================================

.. contents:: Table of Contents
   :depth: 2

.. raw:: pdf

   PageBreak oneColumn

Audience
========

This report is for evaluating the steady-state performance difference between
the ``uio_pci_generic`` and ``vfio-pci`` uPCIe paths in xNVMe across one or
more NVMe devices running concurrently.

.. include:: xnvme.rst

Purpose
========

The benchmark compares xNVMe ``upcie`` results collected from two boot
configurations (``uio_pci_generic`` from a no-IOMMU setup and ``vfio-pci``
from an IOMMU-enabled setup) for the same workload matrix and the same set
of devices.

.. raw:: pdf

   PageBreak

.. include:: testsetup.rst

.. raw:: pdf

   PageBreak

Methodology
===========

Each input run executes ``fio`` (xnvme ioengine, ``upcie`` backend) with one
``--name=devN`` job per device, ``--numjobs=1``, ``--group_reporting=1``.
Each device pins to one CPU (single-bit ``cpumask``); IOMMU pressure scales
by adding devices, not by adding threads per device.

When the runs file specifies ``device_counts`` with multiple values, each
workload is repeated once per N. For single-device cases (N=1) ``xnvmeperf``
is also run as a throughput cross-check; for N>1 the run is fio-only.

Tables and plots in the per-section views below report aggregate metrics
across the N devices: throughput is summed and latency is averaged across
devices. The optional "Per-device" section breaks the aggregate down by BDF
to inspect device-level fairness.

Throughput delta is reported as ``(vfio_iops - uio_iops) / uio_iops * 100``.
A positive delta means VFIO delivered higher throughput. Latency and
tail-latency deltas use the same sign convention.

{% if workloads %}
Workload Matrix
---------------

.. list-table::
   :widths: 18 24 40
   :header-rows: 1

   * - RW
     - IO sizes
     - IO depths
{% for workload in workloads %}
   * - {{ workload.rw }}
     - {{ workload.iosizes }}
     - {{ workload.iodepths }}
{% endfor %}

{% endif %}

Throughput
==========

{% for section in sections %}

{{ section.rw }} / {{ section.iosize }} bytes / N={{ section.devcount }}
-----------------------------------------------------------------------

.. image:: {{ section.plots["iops"] }}
   :align: center
   :width: 85%

.. list-table::
   :widths: 12 14 14 12 12 12
   :header-rows: 1

   * - IO depth
     - UIO IOPS
     - VFIO IOPS
     - Delta %
     - UIO CV %
     - VFIO CV %
{% for row in section.rows %}
   * - {{ row.iodepth }}
     - {{ row.uio_iops }}
     - {{ row.vfio_iops }}
     - {{ row.iops_delta_pct }}
     - {{ row.uio_iops_cv }}
     - {{ row.vfio_iops_cv }}
{% endfor %}

{% if section.devcount == 1 and "xnvmeperf_iops" in section.plots %}
xnvmeperf cross-check (single-device only):

.. image:: {{ section.plots["xnvmeperf_iops"] }}
   :align: center
   :width: 85%

.. list-table::
   :widths: 12 14 14 12
   :header-rows: 1

   * - IO depth
     - UIO xnvmeperf IOPS
     - VFIO xnvmeperf IOPS
     - Delta %
{% for row in section.rows %}{% if row.has_xnvmeperf %}
   * - {{ row.iodepth }}
     - {{ row.uio_xnvmeperf_iops }}
     - {{ row.vfio_xnvmeperf_iops }}
     - {{ row.xnvmeperf_iops_delta_pct }}
{% endif %}{% endfor %}
{% endif %}

{% endfor %}

Throughput Summary
==================

.. list-table::
   :widths: 10 10 10 8 14 14 14
   :header-rows: 1

   * - RW
     - IO size
     - IO depth
     - N
     - UIO IOPS
     - VFIO IOPS
     - Delta %
{% for row in rows %}
   * - {{ row.rw }}
     - {{ row.iosize }}
     - {{ row.iodepth }}
     - {{ row.devcount }}
     - {{ row.uio_iops }}
     - {{ row.vfio_iops }}
     - {{ row.iops_delta_pct }}
{% endfor %}

Latency
=======

{% for section in sections %}

{{ section.rw }} / {{ section.iosize }} bytes / N={{ section.devcount }}
-----------------------------------------------------------------------

.. image:: {{ section.plots["latency"] }}
   :align: center
   :width: 85%

.. list-table::
   :widths: 12 14 14 12
   :header-rows: 1

   * - IO depth
     - UIO us
     - VFIO us
     - Delta %
{% for row in section.rows %}
   * - {{ row.iodepth }}
     - {{ row.uio_lat_us }}
     - {{ row.vfio_lat_us }}
     - {{ row.lat_delta_pct }}
{% endfor %}

{% for percentile, label in tail_latencies %}

.. image:: {{ section.plots[percentile ~ "_latency"] }}
   :align: center
   :width: 85%

.. list-table::
   :widths: 12 14 14 12
   :header-rows: 1

   * - IO depth
     - UIO {{ label }} us
     - VFIO {{ label }} us
     - Delta %
{% for row in section.rows %}
   * - {{ row.iodepth }}
     - {{ row["uio_" ~ percentile ~ "_us"] }}
     - {{ row["vfio_" ~ percentile ~ "_us"] }}
     - {{ row[percentile ~ "_delta_pct"] }}
{% endfor %}

{% endfor %}

{% endfor %}

{% if per_device %}
Per-device Throughput (N > 1)
=============================

For workloads with N > 1 the table below breaks the aggregate down by
device to inspect per-device fairness.

.. list-table::
   :widths: 8 8 8 6 14 11 11 11 11 11
   :header-rows: 1

   * - RW
     - IO size
     - IO depth
     - N
     - Device
     - UIO IOPS
     - VFIO IOPS
     - Delta %
     - UIO us
     - VFIO us
{% for group in per_device %}{% for row in group.rows %}
   * - {{ row.rw }}
     - {{ row.iosize }}
     - {{ row.iodepth }}
     - {{ row.devcount }}
     - {{ row.dev }}
     - {{ row.uio_iops }}
     - {{ row.vfio_iops }}
     - {{ row.iops_delta_pct }}
     - {{ row.uio_lat_us }}
     - {{ row.vfio_lat_us }}
{% endfor %}{% endfor %}

{% endif %}
