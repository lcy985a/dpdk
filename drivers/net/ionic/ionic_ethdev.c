/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 * Copyright(c) 2018-2019 Pensando Systems, Inc. All rights reserved.
 */

#include <rte_pci.h>
#include <rte_bus_pci.h>
#include <rte_ethdev.h>
#include <rte_ethdev_driver.h>
#include <rte_malloc.h>
#include <rte_ethdev_pci.h>

#include "ionic_logs.h"
#include "ionic.h"
#include "ionic_dev.h"
#include "ionic_mac_api.h"
#include "ionic_lif.h"
#include "ionic_ethdev.h"
#include "ionic_rxtx.h"

static int  eth_ionic_dev_init(struct rte_eth_dev *eth_dev, void *init_params);
static int  eth_ionic_dev_uninit(struct rte_eth_dev *eth_dev);
static int  ionic_dev_info_get(struct rte_eth_dev *eth_dev,
	struct rte_eth_dev_info *dev_info);
static int  ionic_dev_configure(struct rte_eth_dev *dev);
static int  ionic_dev_mtu_set(struct rte_eth_dev *dev, uint16_t mtu);
static int  ionic_dev_start(struct rte_eth_dev *dev);
static void ionic_dev_stop(struct rte_eth_dev *dev);
static void ionic_dev_close(struct rte_eth_dev *dev);
static int  ionic_dev_set_link_up(struct rte_eth_dev *dev);
static int  ionic_dev_set_link_down(struct rte_eth_dev *dev);
static int  ionic_dev_link_update(struct rte_eth_dev *eth_dev,
	int wait_to_complete);
static int  ionic_flow_ctrl_get(struct rte_eth_dev *eth_dev,
	struct rte_eth_fc_conf *fc_conf);
static int  ionic_flow_ctrl_set(struct rte_eth_dev *eth_dev,
	struct rte_eth_fc_conf *fc_conf);
static int  ionic_vlan_offload_set(struct rte_eth_dev *eth_dev, int mask);

int ionic_logtype;

static const struct rte_pci_id pci_id_ionic_map[] = {
	{ RTE_PCI_DEVICE(IONIC_PENSANDO_VENDOR_ID, IONIC_DEV_ID_ETH_PF) },
	{ RTE_PCI_DEVICE(IONIC_PENSANDO_VENDOR_ID, IONIC_DEV_ID_ETH_VF) },
	{ RTE_PCI_DEVICE(IONIC_PENSANDO_VENDOR_ID, IONIC_DEV_ID_ETH_MGMT) },
	{ .vendor_id = 0, /* sentinel */ },
};

static const struct rte_eth_desc_lim rx_desc_lim = {
	.nb_max = IONIC_MAX_RING_DESC,
	.nb_min = IONIC_MIN_RING_DESC,
	.nb_align = 1,
};

static const struct rte_eth_desc_lim tx_desc_lim = {
	.nb_max = IONIC_MAX_RING_DESC,
	.nb_min = IONIC_MIN_RING_DESC,
	.nb_align = 1,
	.nb_seg_max = IONIC_TX_MAX_SG_ELEMS,
	.nb_mtu_seg_max = IONIC_TX_MAX_SG_ELEMS,
};

static const struct eth_dev_ops ionic_eth_dev_ops = {
	.dev_infos_get          = ionic_dev_info_get,
	.dev_configure          = ionic_dev_configure,
	.mtu_set                = ionic_dev_mtu_set,
	.dev_start              = ionic_dev_start,
	.dev_stop               = ionic_dev_stop,
	.dev_close              = ionic_dev_close,
	.link_update            = ionic_dev_link_update,
	.dev_set_link_up        = ionic_dev_set_link_up,
	.dev_set_link_down      = ionic_dev_set_link_down,
	.mac_addr_add           = ionic_dev_add_mac,
	.mac_addr_remove        = ionic_dev_remove_mac,
	.mac_addr_set           = ionic_dev_set_mac,
	.vlan_filter_set        = ionic_dev_vlan_filter_set,
	.promiscuous_enable     = ionic_dev_promiscuous_enable,
	.promiscuous_disable    = ionic_dev_promiscuous_disable,
	.allmulticast_enable    = ionic_dev_allmulticast_enable,
	.allmulticast_disable   = ionic_dev_allmulticast_disable,
	.flow_ctrl_get          = ionic_flow_ctrl_get,
	.flow_ctrl_set          = ionic_flow_ctrl_set,
	.rxq_info_get           = ionic_rxq_info_get,
	.txq_info_get           = ionic_txq_info_get,
	.rx_queue_setup         = ionic_dev_rx_queue_setup,
	.rx_queue_release       = ionic_dev_rx_queue_release,
	.rx_queue_start	        = ionic_dev_rx_queue_start,
	.rx_queue_stop          = ionic_dev_rx_queue_stop,
	.tx_queue_setup         = ionic_dev_tx_queue_setup,
	.tx_queue_release       = ionic_dev_tx_queue_release,
	.tx_queue_start	        = ionic_dev_tx_queue_start,
	.tx_queue_stop          = ionic_dev_tx_queue_stop,
	.vlan_offload_set       = ionic_vlan_offload_set,
};

/*
 * Set device link up, enable tx.
 */
static int
ionic_dev_set_link_up(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_dev *idev = &adapter->idev;
	int err;

	IONIC_PRINT_CALL();

	ionic_dev_cmd_port_state(idev, IONIC_PORT_ADMIN_STATE_UP);

	err = ionic_dev_cmd_wait_check(idev, IONIC_DEVCMD_TIMEOUT);
	if (err) {
		IONIC_PRINT(WARNING, "Failed to bring port UP");
		return err;
	}

	return 0;
}

/*
 * Set device link down, disable tx.
 */
static int
ionic_dev_set_link_down(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_dev *idev = &adapter->idev;
	int err;

	IONIC_PRINT_CALL();

	ionic_dev_cmd_port_state(idev, IONIC_PORT_ADMIN_STATE_DOWN);

	err = ionic_dev_cmd_wait_check(idev, IONIC_DEVCMD_TIMEOUT);
	if (err) {
		IONIC_PRINT(WARNING, "Failed to bring port DOWN");
		return err;
	}

	return 0;
}

static int
ionic_dev_link_update(struct rte_eth_dev *eth_dev,
		int wait_to_complete __rte_unused)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct rte_eth_link link;

	IONIC_PRINT_CALL();

	/* Initialize */
	memset(&link, 0, sizeof(link));
	link.link_autoneg = ETH_LINK_AUTONEG;

	if (!adapter->link_up) {
		/* Interface is down */
		link.link_status = ETH_LINK_DOWN;
		link.link_duplex = ETH_LINK_HALF_DUPLEX;
		link.link_speed = ETH_SPEED_NUM_NONE;
	} else {
		/* Interface is up */
		link.link_status = ETH_LINK_UP;
		link.link_duplex = ETH_LINK_FULL_DUPLEX;
		switch (adapter->link_speed) {
		case  10000:
			link.link_speed = ETH_SPEED_NUM_10G;
			break;
		case  25000:
			link.link_speed = ETH_SPEED_NUM_25G;
			break;
		case  40000:
			link.link_speed = ETH_SPEED_NUM_40G;
			break;
		case  50000:
			link.link_speed = ETH_SPEED_NUM_50G;
			break;
		case 100000:
			link.link_speed = ETH_SPEED_NUM_100G;
			break;
		default:
			link.link_speed = ETH_SPEED_NUM_NONE;
			break;
		}
	}

	return rte_eth_linkstatus_set(eth_dev, &link);
}

/**
 * Interrupt handler triggered by NIC for handling
 * specific interrupt.
 *
 * @param param
 *  The address of parameter registered before.
 *
 * @return
 *  void
 */
static void
ionic_dev_interrupt_handler(void *param)
{
	struct ionic_adapter *adapter = (struct ionic_adapter *)param;
	uint32_t i;

	IONIC_PRINT(DEBUG, "->");

	for (i = 0; i < adapter->nlifs; i++) {
		if (adapter->lifs[i])
			ionic_notifyq_handler(adapter->lifs[i], -1);
	}
}

static int
ionic_dev_mtu_set(struct rte_eth_dev *eth_dev, uint16_t mtu)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	uint32_t max_frame_size;
	int err;

	IONIC_PRINT_CALL();

	/*
	 * Note: mtu check against IONIC_MIN_MTU, IONIC_MAX_MTU
	 * is done by the the API.
	 */

	/*
	 * Max frame size is MTU + Ethernet header + VLAN + QinQ
	 * (plus ETHER_CRC_LEN if the adapter is able to keep CRC)
	 */
	max_frame_size = mtu + RTE_ETHER_HDR_LEN + 4 + 4;

	if (eth_dev->data->dev_conf.rxmode.max_rx_pkt_len < max_frame_size)
		return -EINVAL;

	err = ionic_lif_change_mtu(lif, mtu);
	if (err)
		return err;

	return 0;
}

static int
ionic_dev_info_get(struct rte_eth_dev *eth_dev,
		struct rte_eth_dev_info *dev_info)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_identity *ident = &adapter->ident;

	IONIC_PRINT_CALL();

	dev_info->max_rx_queues = (uint16_t)
		ident->lif.eth.config.queue_count[IONIC_QTYPE_RXQ];
	dev_info->max_tx_queues = (uint16_t)
		ident->lif.eth.config.queue_count[IONIC_QTYPE_TXQ];
	/* Also add ETHER_CRC_LEN if the adapter is able to keep CRC */
	dev_info->min_rx_bufsize = IONIC_MIN_MTU + RTE_ETHER_HDR_LEN;
	dev_info->max_rx_pktlen = IONIC_MAX_MTU + RTE_ETHER_HDR_LEN;
	dev_info->max_mac_addrs = adapter->max_mac_addrs;
	dev_info->min_mtu = IONIC_MIN_MTU;
	dev_info->max_mtu = IONIC_MAX_MTU;

	dev_info->speed_capa =
		ETH_LINK_SPEED_10G |
		ETH_LINK_SPEED_25G |
		ETH_LINK_SPEED_40G |
		ETH_LINK_SPEED_50G |
		ETH_LINK_SPEED_100G;

	/*
	 * Per-queue capabilities. Actually most of the offloads are enabled
	 * by default on the port and can be used on selected queues (by adding
	 * packet flags at runtime when required)
	 */

	dev_info->rx_queue_offload_capa =
		DEV_RX_OFFLOAD_IPV4_CKSUM |
		DEV_RX_OFFLOAD_UDP_CKSUM |
		DEV_RX_OFFLOAD_TCP_CKSUM |
		0;

	dev_info->tx_queue_offload_capa =
		DEV_TX_OFFLOAD_VLAN_INSERT |
		0;

	/*
	 * Per-port capabilities
	 * See ionic_set_features to request and check supported features
	 */

	dev_info->rx_offload_capa = dev_info->rx_queue_offload_capa |
		DEV_RX_OFFLOAD_JUMBO_FRAME |
		DEV_RX_OFFLOAD_VLAN_FILTER |
		DEV_RX_OFFLOAD_VLAN_STRIP |
		DEV_RX_OFFLOAD_SCATTER |
		0;

	dev_info->tx_offload_capa = dev_info->tx_queue_offload_capa |
		DEV_TX_OFFLOAD_MULTI_SEGS |
		DEV_TX_OFFLOAD_TCP_TSO |
		0;

	dev_info->rx_desc_lim = rx_desc_lim;
	dev_info->tx_desc_lim = tx_desc_lim;

	/* Driver-preferred Rx/Tx parameters */
	dev_info->default_rxportconf.burst_size = 32;
	dev_info->default_txportconf.burst_size = 32;
	dev_info->default_rxportconf.nb_queues = 1;
	dev_info->default_txportconf.nb_queues = 1;
	dev_info->default_rxportconf.ring_size = IONIC_DEF_TXRX_DESC;
	dev_info->default_txportconf.ring_size = IONIC_DEF_TXRX_DESC;

	return 0;
}

static int
ionic_flow_ctrl_get(struct rte_eth_dev *eth_dev,
		struct rte_eth_fc_conf *fc_conf)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_dev *idev = &adapter->idev;

	if (idev->port_info) {
		fc_conf->autoneg = idev->port_info->config.an_enable;

		if (idev->port_info->config.pause_type)
			fc_conf->mode = RTE_FC_FULL;
		else
			fc_conf->mode = RTE_FC_NONE;
	}

	return 0;
}

static int
ionic_flow_ctrl_set(struct rte_eth_dev *eth_dev,
		struct rte_eth_fc_conf *fc_conf)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_dev *idev = &adapter->idev;
	uint8_t pause_type = IONIC_PORT_PAUSE_TYPE_NONE;
	uint8_t an_enable;

	switch (fc_conf->mode) {
	case RTE_FC_NONE:
		pause_type = IONIC_PORT_PAUSE_TYPE_NONE;
		break;
	case RTE_FC_FULL:
		pause_type = IONIC_PORT_PAUSE_TYPE_LINK;
		break;
	case RTE_FC_RX_PAUSE:
	case RTE_FC_TX_PAUSE:
		return -ENOTSUP;
	}

	an_enable = fc_conf->autoneg;

	ionic_dev_cmd_port_pause(idev, pause_type);
	ionic_dev_cmd_port_autoneg(idev, an_enable);

	return 0;
}

static int
ionic_vlan_offload_set(struct rte_eth_dev *eth_dev, int mask)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct rte_eth_rxmode *rxmode;
	rxmode = &eth_dev->data->dev_conf.rxmode;
	int i;

	if (mask & ETH_VLAN_STRIP_MASK) {
		if (rxmode->offloads & DEV_RX_OFFLOAD_VLAN_STRIP) {
			for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
				struct ionic_qcq *rxq =
					eth_dev->data->rx_queues[i];
				rxq->offloads |= DEV_RX_OFFLOAD_VLAN_STRIP;
			}
			lif->features |= IONIC_ETH_HW_VLAN_RX_STRIP;
		} else {
			for (i = 0; i < eth_dev->data->nb_rx_queues; i++) {
				struct ionic_qcq *rxq =
					eth_dev->data->rx_queues[i];
				rxq->offloads &= ~DEV_RX_OFFLOAD_VLAN_STRIP;
			}
			lif->features &= ~IONIC_ETH_HW_VLAN_RX_STRIP;
		}
	}

	if (mask & ETH_VLAN_FILTER_MASK) {
		if (rxmode->offloads & DEV_RX_OFFLOAD_VLAN_FILTER)
			lif->features |= IONIC_ETH_HW_VLAN_RX_FILTER;
		else
			lif->features &= ~IONIC_ETH_HW_VLAN_RX_FILTER;
	}

	ionic_lif_set_features(lif);

	return 0;
}

static int
ionic_dev_configure(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	int err;

	IONIC_PRINT_CALL();

	err = ionic_lif_configure(lif);
	if (err) {
		IONIC_PRINT(ERR, "Cannot configure LIF: %d", err);
		return err;
	}

	return 0;
}

static inline uint32_t
ionic_parse_link_speeds(uint16_t link_speeds)
{
	if (link_speeds & ETH_LINK_SPEED_100G)
		return 100000;
	else if (link_speeds & ETH_LINK_SPEED_50G)
		return 50000;
	else if (link_speeds & ETH_LINK_SPEED_40G)
		return 40000;
	else if (link_speeds & ETH_LINK_SPEED_25G)
		return 25000;
	else if (link_speeds & ETH_LINK_SPEED_10G)
		return 10000;
	else
		return 0;
}

/*
 * Configure device link speed and setup link.
 * It returns 0 on success.
 */
static int
ionic_dev_start(struct rte_eth_dev *eth_dev)
{
	struct rte_eth_conf *dev_conf = &eth_dev->data->dev_conf;
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;
	struct ionic_dev *idev = &adapter->idev;
	uint32_t allowed_speeds;
	int err;

	IONIC_PRINT_CALL();

	allowed_speeds =
		ETH_LINK_SPEED_FIXED |
		ETH_LINK_SPEED_10G |
		ETH_LINK_SPEED_25G |
		ETH_LINK_SPEED_40G |
		ETH_LINK_SPEED_50G |
		ETH_LINK_SPEED_100G;

	if (dev_conf->link_speeds & ~allowed_speeds) {
		IONIC_PRINT(ERR, "Invalid link setting");
		return -EINVAL;
	}

	err = ionic_lif_start(lif);
	if (err) {
		IONIC_PRINT(ERR, "Cannot start LIF: %d", err);
		return err;
	}

	if (eth_dev->data->dev_conf.link_speeds & ETH_LINK_SPEED_FIXED) {
		uint32_t speed = ionic_parse_link_speeds(dev_conf->link_speeds);

		if (speed)
			ionic_dev_cmd_port_speed(idev, speed);
	}

	ionic_dev_link_update(eth_dev, 0);

	return 0;
}

/*
 * Stop device: disable rx and tx functions to allow for reconfiguring.
 */
static void
ionic_dev_stop(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	int err;

	IONIC_PRINT_CALL();

	err = ionic_lif_stop(lif);
	if (err)
		IONIC_PRINT(ERR, "Cannot stop LIF: %d", err);
}

/*
 * Reset and stop device.
 */
static void
ionic_dev_close(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	int err;

	IONIC_PRINT_CALL();

	err = ionic_lif_stop(lif);
	if (err) {
		IONIC_PRINT(ERR, "Cannot stop LIF: %d", err);
		return;
	}

	err = eth_ionic_dev_uninit(eth_dev);
	if (err) {
		IONIC_PRINT(ERR, "Cannot destroy LIF: %d", err);
		return;
	}
}

static int
eth_ionic_dev_init(struct rte_eth_dev *eth_dev, void *init_params)
{
	struct rte_pci_device *pci_dev = RTE_ETH_DEV_TO_PCI(eth_dev);
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = (struct ionic_adapter *)init_params;
	int err;

	IONIC_PRINT_CALL();

	eth_dev->dev_ops = &ionic_eth_dev_ops;
	eth_dev->rx_pkt_burst = &ionic_recv_pkts;
	eth_dev->tx_pkt_burst = &ionic_xmit_pkts;
	eth_dev->tx_pkt_prepare = &ionic_prep_pkts;

	/* Multi-process not supported, primary does initialization anyway */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	lif->index = adapter->nlifs;
	lif->eth_dev = eth_dev;
	lif->adapter = adapter;
	adapter->lifs[adapter->nlifs] = lif;

	IONIC_PRINT(DEBUG, "Up to %u MAC addresses supported",
		adapter->max_mac_addrs);

	/* Allocate memory for storing MAC addresses */
	eth_dev->data->mac_addrs = rte_zmalloc("ionic",
		RTE_ETHER_ADDR_LEN * adapter->max_mac_addrs, 0);

	if (eth_dev->data->mac_addrs == NULL) {
		IONIC_PRINT(ERR, "Failed to allocate %u bytes needed to "
			"store MAC addresses",
			RTE_ETHER_ADDR_LEN * adapter->max_mac_addrs);
		err = -ENOMEM;
		goto err;
	}

	err = ionic_lif_alloc(lif);
	if (err) {
		IONIC_PRINT(ERR, "Cannot allocate LIFs: %d, aborting",
			err);
		goto err;
	}

	err = ionic_lif_init(lif);
	if (err) {
		IONIC_PRINT(ERR, "Cannot init LIFs: %d, aborting", err);
		goto err_free_lif;
	}

	/* Copy the MAC address */
	rte_ether_addr_copy((struct rte_ether_addr *)lif->mac_addr,
		&eth_dev->data->mac_addrs[0]);

	IONIC_PRINT(DEBUG, "Port %u initialized", eth_dev->data->port_id);

	return 0;

err_free_lif:
	ionic_lif_free(lif);
err:
	return err;
}

static int
eth_ionic_dev_uninit(struct rte_eth_dev *eth_dev)
{
	struct ionic_lif *lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
	struct ionic_adapter *adapter = lif->adapter;

	IONIC_PRINT_CALL();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return 0;

	adapter->lifs[lif->index] = NULL;

	ionic_lif_deinit(lif);
	ionic_lif_free(lif);

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;
	eth_dev->tx_pkt_prepare = NULL;

	return 0;
}

static int
ionic_configure_intr(struct ionic_adapter *adapter)
{
	struct rte_pci_device *pci_dev = adapter->pci_dev;
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;
	int err;

	IONIC_PRINT(DEBUG, "Configuring %u intrs", adapter->nintrs);

	if (rte_intr_efd_enable(intr_handle, adapter->nintrs)) {
		IONIC_PRINT(ERR, "Fail to create eventfd");
		return -1;
	}

	if (rte_intr_dp_is_en(intr_handle))
		IONIC_PRINT(DEBUG,
			"Packet I/O interrupt on datapath is enabled");

	if (!intr_handle->intr_vec) {
		intr_handle->intr_vec = rte_zmalloc("intr_vec",
			adapter->nintrs * sizeof(int), 0);

		if (!intr_handle->intr_vec) {
			IONIC_PRINT(ERR, "Failed to allocate %u vectors",
				adapter->nintrs);
			return -ENOMEM;
		}
	}

	err = rte_intr_callback_register(intr_handle,
		ionic_dev_interrupt_handler,
		adapter);

	if (err) {
		IONIC_PRINT(ERR,
			"Failure registering interrupts handler (%d)",
			err);
		return err;
	}

	/* enable intr mapping */
	err = rte_intr_enable(intr_handle);

	if (err) {
		IONIC_PRINT(ERR, "Failure enabling interrupts (%d)", err);
		return err;
	}

	return 0;
}

static void
ionic_unconfigure_intr(struct ionic_adapter *adapter)
{
	struct rte_pci_device *pci_dev = adapter->pci_dev;
	struct rte_intr_handle *intr_handle = &pci_dev->intr_handle;

	rte_intr_disable(intr_handle);

	rte_intr_callback_unregister(intr_handle,
		ionic_dev_interrupt_handler,
		adapter);
}

static int
eth_ionic_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
		struct rte_pci_device *pci_dev)
{
	char name[RTE_ETH_NAME_MAX_LEN];
	struct rte_mem_resource *resource;
	struct ionic_adapter *adapter;
	struct ionic_hw *hw;
	unsigned long i;
	int err;

	/* Check structs (trigger error at compilation time) */
	ionic_struct_size_checks();

	/* Multi-process not supported */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY) {
		err = -EPERM;
		goto err;
	}

	IONIC_PRINT(DEBUG, "Initializing device %s",
		pci_dev->device.name);

	adapter = rte_zmalloc("ionic", sizeof(*adapter), 0);
	if (!adapter) {
		IONIC_PRINT(ERR, "OOM");
		err = -ENOMEM;
		goto err;
	}

	adapter->pci_dev = pci_dev;
	hw = &adapter->hw;

	hw->device_id = pci_dev->id.device_id;
	hw->vendor_id = pci_dev->id.vendor_id;

	err = ionic_init_mac(hw);
	if (err != 0) {
		IONIC_PRINT(ERR, "Mac init failed: %d", err);
		err = -EIO;
		goto err_free_adapter;
	}

	adapter->is_mgmt_nic = (pci_dev->id.device_id == IONIC_DEV_ID_ETH_MGMT);

	adapter->num_bars = 0;
	for (i = 0; i < PCI_MAX_RESOURCE && i < IONIC_BARS_MAX; i++) {
		resource = &pci_dev->mem_resource[i];
		if (resource->phys_addr == 0 || resource->len == 0)
			continue;
		adapter->bars[adapter->num_bars].vaddr = resource->addr;
		adapter->bars[adapter->num_bars].bus_addr = resource->phys_addr;
		adapter->bars[adapter->num_bars].len = resource->len;
		adapter->num_bars++;
	}

	/* Discover ionic dev resources */

	err = ionic_setup(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot setup device: %d, aborting", err);
		goto err_free_adapter;
	}

	err = ionic_identify(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot identify device: %d, aborting",
			err);
		goto err_free_adapter;
	}

	err = ionic_init(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot init device: %d, aborting", err);
		goto err_free_adapter;
	}

	/* Configure the ports */
	err = ionic_port_identify(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot identify port: %d, aborting",
			err);
		goto err_free_adapter;
	}

	err = ionic_port_init(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot init port: %d, aborting", err);
		goto err_free_adapter;
	}

	/* Configure LIFs */
	err = ionic_lif_identify(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot identify lif: %d, aborting", err);
		goto err_free_adapter;
	}

	/* Allocate and init LIFs */
	err = ionic_lifs_size(adapter);
	if (err) {
		IONIC_PRINT(ERR, "Cannot size LIFs: %d, aborting", err);
		goto err_free_adapter;
	}

	adapter->max_mac_addrs = adapter->ident.lif.eth.max_ucast_filters;

	adapter->nlifs = 0;
	for (i = 0; i < adapter->ident.dev.nlifs; i++) {
		snprintf(name, sizeof(name), "net_%s_lif_%lu",
			pci_dev->device.name, i);

		err = rte_eth_dev_create(&pci_dev->device, name,
			sizeof(struct ionic_lif),
			NULL, NULL,
			eth_ionic_dev_init, adapter);
		if (err) {
			IONIC_PRINT(ERR, "Cannot create eth device for "
				"ionic lif %s", name);
			break;
		}

		adapter->nlifs++;
	}

	err = ionic_configure_intr(adapter);

	if (err) {
		IONIC_PRINT(ERR, "Failed to configure interrupts");
		goto err_free_adapter;
	}

	return 0;

err_free_adapter:
	rte_free(adapter);
err:
	return err;
}

static int
eth_ionic_pci_remove(struct rte_pci_device *pci_dev __rte_unused)
{
	char name[RTE_ETH_NAME_MAX_LEN];
	struct ionic_adapter *adapter = NULL;
	struct rte_eth_dev *eth_dev;
	struct ionic_lif *lif;
	uint32_t i;

	/* Adapter lookup is using (the first) eth_dev name */
	snprintf(name, sizeof(name), "net_%s_lif_0",
		pci_dev->device.name);

	eth_dev = rte_eth_dev_allocated(name);
	if (eth_dev) {
		lif = IONIC_ETH_DEV_TO_LIF(eth_dev);
		adapter = lif->adapter;
	}

	if (adapter) {
		ionic_unconfigure_intr(adapter);

		for (i = 0; i < adapter->nlifs; i++) {
			lif = adapter->lifs[i];
			rte_eth_dev_destroy(lif->eth_dev, eth_ionic_dev_uninit);
		}

		rte_free(adapter);
	}

	return 0;
}

static struct rte_pci_driver rte_ionic_pmd = {
	.id_table = pci_id_ionic_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_INTR_LSC,
	.probe = eth_ionic_pci_probe,
	.remove = eth_ionic_pci_remove,
};

RTE_PMD_REGISTER_PCI(net_ionic, rte_ionic_pmd);
RTE_PMD_REGISTER_PCI_TABLE(net_ionic, pci_id_ionic_map);
RTE_PMD_REGISTER_KMOD_DEP(net_ionic, "* igb_uio | uio_pci_generic | vfio-pci");

RTE_INIT(ionic_init_log)
{
	ionic_logtype = rte_log_register("pmd.net.ionic");
	if (ionic_logtype >= 0)
		rte_log_set_level(ionic_logtype, RTE_LOG_NOTICE);
}