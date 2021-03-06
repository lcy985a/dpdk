..  SPDX-License-Identifier: BSD-3-Clause
    Copyright(c) 2016-2019 Intel Corporation.

ZUC Crypto Poll Mode Driver
===========================

The ZUC PMD (**librte_pmd_zuc**) provides poll mode crypto driver support for
utilizing `Intel IPSec Multi-buffer library <https://github.com/01org/intel-ipsec-mb>`_
which implements F8 and F9 functions for ZUC EEA3 cipher and EIA3 hash algorithms.

Features
--------

ZUC PMD has support for:

Cipher algorithm:

* RTE_CRYPTO_CIPHER_ZUC_EEA3

Authentication algorithm:

* RTE_CRYPTO_AUTH_ZUC_EIA3

Limitations
-----------

* Chained mbufs are not supported.
* ZUC (EIA3) supported only if hash offset field is byte-aligned.
* ZUC (EEA3) supported only if cipher length, cipher offset fields are byte-aligned.


Installation
------------

To build DPDK with the ZUC_PMD the user is required to download the multi-buffer
library from `here <https://github.com/01org/intel-ipsec-mb>`_
and compile it on their user system before building DPDK.
The latest version of the library supported by this PMD is v0.53, which
can be downloaded from `<https://github.com/01org/intel-ipsec-mb/archive/v0.53.zip>`_.

After downloading the library, the user needs to unpack and compile it
on their system before building DPDK:

.. code-block:: console

    make
    make install

As a reference, the following table shows a mapping between the past DPDK versions
and the external crypto libraries supported by them:

.. _table_zuc_versions:

.. table:: DPDK and external crypto library version compatibility

   =============  ================================
   DPDK version   Crypto library version
   =============  ================================
   16.11 - 19.11  LibSSO ZUC
   20.02+         Multi-buffer library 0.53
   =============  ================================


Initialization
--------------

In order to enable this virtual crypto PMD, user must:

* Build the multi buffer library (explained in Installation section).

* Build DPDK as follows:

.. code-block:: console

	make config T=x86_64-native-linux-gcc
	sed -i 's,\(CONFIG_RTE_LIBRTE_PMD_ZUC\)=n,\1=y,' build/.config
	make

To use the PMD in an application, user must:

* Call rte_vdev_init("crypto_zuc") within the application.

* Use --vdev="crypto_zuc" in the EAL options, which will call rte_vdev_init() internally.

The following parameters (all optional) can be provided in the previous two calls:

* socket_id: Specify the socket where the memory for the device is going to be allocated
  (by default, socket_id will be the socket where the core that is creating the PMD is running on).

* max_nb_queue_pairs: Specify the maximum number of queue pairs in the device (8 by default).

* max_nb_sessions: Specify the maximum number of sessions that can be created (2048 by default).

Example:

.. code-block:: console

    ./l2fwd-crypto -l 1 -n 4 --vdev="crypto_zuc,socket_id=0,max_nb_sessions=128" \
    -- -p 1 --cdev SW --chain CIPHER_ONLY --cipher_algo "zuc-eea3"
