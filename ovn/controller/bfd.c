/* Copyright (c) 2017 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "bfd.h"
#include "gchassis.h"
#include "lport.h"
#include "ovn-controller.h"

#include "lib/hash.h"
#include "lib/sset.h"
#include "lib/util.h"
#include "lib/vswitch-idl.h"
#include "openvswitch/vlog.h"
#include "ovn/lib/ovn-sb-idl.h"
#include "ovn-controller.h"

VLOG_DEFINE_THIS_MODULE(ovn_bfd);

void
bfd_register_ovs_idl(struct ovsdb_idl *ovs_idl)
{
    /* NOTE: this assumes that binding.c has added the
     * ovsrec_interface table */
    ovsdb_idl_add_column(ovs_idl, &ovsrec_interface_col_bfd);
    ovsdb_idl_add_column(ovs_idl, &ovsrec_interface_col_bfd_status);
}


static void
interface_set_bfd(const struct ovsrec_interface *iface, bool bfd_setting)
{
    const char *new_setting = bfd_setting ? "true":"false";
    const char *current_setting = smap_get(&iface->bfd, "enable");
    if (current_setting && !strcmp(current_setting, new_setting)) {
        /* If already set to the desired setting we skip setting it again
         * to avoid flapping to bfd initialization state */
        return;
    }
    const struct smap bfd = SMAP_CONST1(&bfd, "enable", new_setting);
    ovsrec_interface_verify_bfd(iface);
    ovsrec_interface_set_bfd(iface, &bfd);
    VLOG_INFO("%s BFD on interface %s", bfd_setting ? "Enabled" : "Disabled",
                                        iface->name);
}

void
bfd_calculate_active_tunnels(const struct ovsrec_bridge *br_int,
                             struct sset *active_tunnels)
{
    int i;

    for (i = 0; i < br_int->n_ports; i++) {
        const struct ovsrec_port *port_rec = br_int->ports[i];

        if (!strcmp(port_rec->name, br_int->name)) {
            continue;
        }

        int j;
        for (j = 0; j < port_rec->n_interfaces; j++) {
            const struct ovsrec_interface *iface_rec;
            iface_rec = port_rec->interfaces[j];

            /* Check if this is a tunnel interface. */
            if (smap_get(&iface_rec->options, "remote_ip")) {
                /* Add ovn-chassis-id if the bfd_status of the tunnel
                 * is active */
                const char *bfd = smap_get(&iface_rec->bfd, "enable");
                if (bfd && !strcmp(bfd, "true")) {
                    const char *status = smap_get(&iface_rec->bfd_status,
                                                  "state");
                    if (status && !strcmp(status, "up")) {
                        const char *id = smap_get(&port_rec->external_ids,
                                                  "ovn-chassis-id");
                        if (id) {
                            sset_add(active_tunnels, id);
                        }
                    }
                }
            }
        }
    }
}

static void
bfd_calculate_chassis(const struct sbrec_chassis *our_chassis,
                      struct hmap *local_datapaths,
                      const struct chassis_index *chassis_index,
                      struct sset *bfd_chassis)
{
    /* Identify all chassis nodes to which we need to enable bfd.
     * 1) Any chassis hosting the chassisredirect ports for known
     *    router datapaths.
     * 2) Chassis hosting peer datapaths (with ports) connected
     *    to a router datapath  when our chassis is hosting a router
     *    with a chassis redirect port. */
    struct local_datapath *dp;
    HMAP_FOR_EACH (dp, hmap_node, local_datapaths) {
        const char *is_router = smap_get(&dp->datapath->external_ids,
                                         "logical-router");
        bool our_chassis_is_gw_for_dp = false;
        if (is_router) {
            for (size_t j = 0; j < dp->ldatapath->n_lports; j++) {
                const struct sbrec_port_binding *pb = dp->ldatapath->lports[j];
                if (!strcmp(pb->type, "chassisredirect")) {
                    struct ovs_list *gateway_chassis = NULL;
                    gateway_chassis =
                        gateway_chassis_get_ordered(pb, chassis_index);
                    /* we don't need BFD for non-HA  chassisredirect */
                    if (!gateway_chassis ||
                        ovs_list_is_short(gateway_chassis)) {
                        continue;
                    }
                    our_chassis_is_gw_for_dp = gateway_chassis_contains(
                            gateway_chassis, our_chassis);
                    struct gateway_chassis *gwc;
                    LIST_FOR_EACH (gwc, node, gateway_chassis) {
                        if (gwc->db->chassis) {
                            sset_add(bfd_chassis, gwc->db->chassis->name);
                        }
                    }
                    gateway_chassis_destroy(gateway_chassis);
                    break;
                }
            }
        }
        if (our_chassis_is_gw_for_dp) {
            for (size_t i = 0; i < dp->n_peer_dps; i++) {
                const struct ldatapath *pdp = dp->peer_dps[i];
                if (!pdp) {
                    continue;
                }
                for (size_t j = 0; j < pdp->n_lports; j++) {
                    const struct sbrec_port_binding *pb = pdp->lports[j];
                    if (pb->chassis) {
                        /* Gateway node has to enable bfd to all nodes hosting
                         * connected network ports */
                        const char *chassis_name = pb->chassis->name;
                        if (chassis_name) {
                            sset_add(bfd_chassis, chassis_name);
                        }
                    }
                }
            }
        }
    }
}

void
bfd_run(struct controller_ctx *ctx, const struct ovsrec_bridge *br_int,
        const struct sbrec_chassis *chassis_rec, struct hmap *local_datapaths,
        const struct chassis_index *chassis_index)
{

    if (!chassis_rec) {
        return;
    }
    struct sset bfd_chassis = SSET_INITIALIZER(&bfd_chassis);
    bfd_calculate_chassis(chassis_rec, local_datapaths, chassis_index,
                          &bfd_chassis);
    /* Identify tunnels ports(connected to remote chassis id) to enable bfd */
    struct sset tunnels = SSET_INITIALIZER(&tunnels);
    struct sset bfd_ifaces = SSET_INITIALIZER(&bfd_ifaces);
    for (size_t k = 0; k < br_int->n_ports; k++) {
        const char *chassis_id = smap_get(&br_int->ports[k]->external_ids,
                                          "ovn-chassis-id");
        if (chassis_id) {
            char *port_name = br_int->ports[k]->name;
            sset_add(&tunnels, port_name);
            if (sset_contains(&bfd_chassis, chassis_id)) {
                sset_add(&bfd_ifaces, port_name);
            }
        }
    }

    /* Enable or disable bfd */
    const struct ovsrec_interface *iface;
    OVSREC_INTERFACE_FOR_EACH (iface, ctx->ovs_idl) {
        if (sset_contains(&tunnels, iface->name)) {
                interface_set_bfd(
                        iface, sset_contains(&bfd_ifaces, iface->name));
         }
    }

    sset_destroy(&tunnels);
    sset_destroy(&bfd_ifaces);
    sset_destroy(&bfd_chassis);
}
