/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "router_core_private.h"
#include <qpid/dispatch/amqp.h>
#include <stdio.h>

static void qdr_connection_opened_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_connection_closed_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_first_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_second_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);
static void qdr_link_detach_CT(qdr_core_t *core, qdr_action_t *action, bool discard);

ALLOC_DEFINE(qdr_connection_t);
ALLOC_DEFINE(qdr_connection_work_t);

static qd_address_semantics_t qdr_dynamic_semantics = QD_FANOUT_SINGLE | QD_BIAS_CLOSEST | QD_CONGESTION_BACKPRESSURE;
static qd_address_semantics_t qdr_default_semantics = QD_FANOUT_SINGLE | QD_BIAS_SPREAD  | QD_CONGESTION_BACKPRESSURE;

typedef enum {
    QDR_CONDITION_NO_ROUTE_TO_DESTINATION,
    QDR_CONDITION_ROUTED_LINK_LOST,
    QDR_CONDITION_FORBIDDEN
} qdr_condition_t;

//==================================================================================
// Internal Functions
//==================================================================================

qdr_terminus_t *qdr_terminus_router_control(void)
{
    qdr_terminus_t *term = qdr_terminus(0);
    qdr_terminus_add_capability(term, QD_CAPABILITY_ROUTER_CONTROL);
    return term;
}


qdr_terminus_t *qdr_terminus_router_data(void)
{
    qdr_terminus_t *term = qdr_terminus(0);
    qdr_terminus_add_capability(term, QD_CAPABILITY_ROUTER_DATA);
    return term;
}


//==================================================================================
// Interface Functions
//==================================================================================

qdr_connection_t *qdr_connection_opened(qdr_core_t *core, bool incoming, qdr_connection_role_t role, const char *label)
{
    qdr_action_t     *action = qdr_action(qdr_connection_opened_CT);
    qdr_connection_t *conn   = new_qdr_connection_t();

    ZERO(conn);
    conn->core         = core;
    conn->user_context = 0;
    conn->incoming     = incoming;
    conn->role         = role;
    conn->label        = label;
    conn->mask_bit     = -1;
    DEQ_INIT(conn->links);
    DEQ_INIT(conn->work_list);
    conn->work_lock    = sys_mutex();

    action->args.connection.conn = conn;
    qdr_action_enqueue(core, action);

    return conn;
}


void qdr_connection_closed(qdr_connection_t *conn)
{
    qdr_action_t *action = qdr_action(qdr_connection_closed_CT);
    action->args.connection.conn = conn;
    qdr_action_enqueue(conn->core, action);
}


void qdr_connection_set_context(qdr_connection_t *conn, void *context)
{
    if (conn)
        conn->user_context = context;
}


void *qdr_connection_get_context(const qdr_connection_t *conn)
{
    return conn ? conn->user_context : 0;
}


void qdr_connection_process(qdr_connection_t *conn)
{
    qdr_connection_work_list_t  work_list;
    qdr_core_t                 *core = conn->core;

    sys_mutex_lock(conn->work_lock);
    DEQ_MOVE(conn->work_list, work_list);
    sys_mutex_unlock(conn->work_lock);

    qdr_connection_work_t *work = DEQ_HEAD(work_list);
    while (work) {
        DEQ_REMOVE_HEAD(work_list);

        switch (work->work_type) {
        case QDR_CONNECTION_WORK_FIRST_ATTACH :
            core->first_attach_handler(core->user_context, conn, work->link, work->source, work->target);
            break;

        case QDR_CONNECTION_WORK_SECOND_ATTACH :
            core->second_attach_handler(core->user_context, work->link, work->source, work->target);
            break;

        case QDR_CONNECTION_WORK_DETACH :
            core->detach_handler(core->user_context, work->link, work->condition);
            break;
        }

        free_qdr_connection_work_t(work);
        work = DEQ_HEAD(work_list);
    }
}


void qdr_link_set_context(qdr_link_t *link, void *context)
{
    if (link)
        link->user_context = context;
}


void *qdr_link_get_context(const qdr_link_t *link)
{
    return link ? link->user_context : 0;
}


qd_link_type_t qdr_link_type(const qdr_link_t *link)
{
    return link->link_type;
}


qd_direction_t qdr_link_direction(const qdr_link_t *link)
{
    return link->link_direction;
}


qdr_link_t *qdr_link_first_attach(qdr_connection_t *conn, qd_direction_t dir, qdr_terminus_t *source, qdr_terminus_t *target)
{
    qdr_action_t *action = qdr_action(qdr_link_first_attach_CT);
    qdr_link_t   *link   = new_qdr_link_t();

    ZERO(link);
    link->core = conn->core;
    link->conn = conn;

    action->args.connection.conn   = conn;
    action->args.connection.link   = link;
    action->args.connection.dir    = dir;
    action->args.connection.source = source;
    action->args.connection.target = target;
    qdr_action_enqueue(conn->core, action);

    return link;
}


void qdr_link_second_attach(qdr_link_t *link, qdr_terminus_t *source, qdr_terminus_t *target)
{
    qdr_action_t *action = qdr_action(qdr_link_second_attach_CT);

    action->args.connection.link   = link;
    action->args.connection.source = source;
    action->args.connection.target = target;
    qdr_action_enqueue(link->core, action);
}


void qdr_link_detach(qdr_link_t *link, pn_condition_t *condition)
{
    qdr_action_t *action = qdr_action(qdr_link_detach_CT);

    action->args.connection.link      = link;
    action->args.connection.condition = condition;
    qdr_action_enqueue(link->core, action);
}


void qdr_connection_handlers(qdr_core_t                *core,
                             void                      *context,
                             qdr_connection_activate_t  activate,
                             qdr_link_first_attach_t    first_attach,
                             qdr_link_second_attach_t   second_attach,
                             qdr_link_detach_t          detach)
{
    core->user_context          = context;
    core->activate_handler      = activate;
    core->first_attach_handler  = first_attach;
    core->second_attach_handler = second_attach;
    core->detach_handler        = detach;
}


//==================================================================================
// In-Thread Functions
//==================================================================================

static void qdr_connection_enqueue_work_CT(qdr_core_t            *core,
                                           qdr_connection_t      *conn,
                                           qdr_connection_work_t *work)
{
    sys_mutex_lock(conn->work_lock);
    DEQ_INSERT_TAIL(conn->work_list, work);
    bool notify = DEQ_SIZE(conn->work_list) == 1;
    sys_mutex_unlock(conn->work_lock);

    if (notify)
        core->activate_handler(core->user_context, conn);
}


static qdr_link_t *qdr_create_link_CT(qdr_core_t       *core,
                                      qdr_connection_t *conn,
                                      qd_link_type_t    link_type,
                                      qd_direction_t    dir,
                                      qdr_terminus_t   *source,
                                      qdr_terminus_t   *target)
{
    //
    // Create a new link, initiated by the router core.  This will involve issuing a first-attach outbound.
    //
    qdr_link_t *link = new_qdr_link_t();
    ZERO(link);

    link->core           = core;
    link->user_context   = 0;
    link->conn           = conn;
    link->link_type      = link_type;
    link->link_direction = dir;

    qdr_connection_work_t *work = new_qdr_connection_work_t();
    ZERO(work);
    work->work_type = QDR_CONNECTION_WORK_FIRST_ATTACH;
    work->link      = link;
    work->source    = source;
    work->target    = target;

    qdr_connection_enqueue_work_CT(core, conn, work);
    return link;
}


static void qdr_link_reject_CT(qdr_core_t *core, qdr_link_t *link, qdr_condition_t condition)
{
}


static void qdr_link_accept_CT(qdr_core_t *core, qdr_link_t *link)
{
}


static void qdr_forward_first_attach_CT(qdr_core_t *core, qdr_link_t *link, qdr_address_t *addr)
{
}


/**
 * Generate a temporary routable address for a destination connected to this
 * router node.
 */
static void qdr_generate_temp_addr(qdr_core_t *core, char *buffer, size_t length)
{
    static const char *table = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_";
    char     discriminator[16];
    long int rnd1 = random();
    long int rnd2 = random();
    long int rnd3 = random();
    int      idx;
    int      cursor = 0;

    for (idx = 0; idx < 5; idx++) {
        discriminator[cursor++] = table[(rnd1 >> (idx * 6)) & 63];
        discriminator[cursor++] = table[(rnd2 >> (idx * 6)) & 63];
        discriminator[cursor++] = table[(rnd3 >> (idx * 6)) & 63];
    }
    discriminator[cursor] = '\0';

    snprintf(buffer, length, "amqp:/_topo/%s/%s/temp.%s", core->router_area, core->router_id, discriminator);
}


static char qdr_prefix_for_dir(qd_direction_t dir)
{
    return (dir == QD_INCOMING) ? 'C' : 'D';
}


static qd_address_semantics_t qdr_semantics_for_address(qdr_core_t *core, qd_field_iterator_t *iter)
{
    qdr_address_t *addr = 0;

    //
    // Question: Should we use a new prefix for configuration?
    //
    qd_hash_retrieve_prefix(core->addr_hash, iter, (void**) addr);
    return addr ? addr->semantics : qdr_default_semantics;
}


/**
 * qdr_lookup_terminus_address_CT
 *
 * Lookup a terminus address in the route table and possibly create a new address
 * if no match is found.
 *
 * @param core Pointer to the core object
 * @param dir Direction of the link for the terminus
 * @param terminus The terminus containing the addressing information to be looked up
 * @param create_if_not_found Iff true, return a pointer to a newly created address record
 * @param accept_dynamic Iff true, honor the dynamic flag by creating a dynamic address
 * @param [out] link_route True iff the lookup indicates that an attach should be routed
 * @return Pointer to an address record or 0 if none is found
 */
static qdr_address_t *qdr_lookup_terminus_address_CT(qdr_core_t     *core,
                                                     qd_direction_t  dir,
                                                     qdr_terminus_t *terminus,
                                                     bool            create_if_not_found,
                                                     bool            accept_dynamic,
                                                     bool           *link_route)
{
    qdr_address_t *addr = 0;

    //
    // Unless expressly stated, link routing is not indicated for this terminus.
    //
    *link_route = false;

    if (qdr_terminus_is_dynamic(terminus)) {
        //
        // The terminus is dynamic.  Check to see if there is an address provided
        // in the dynamic node properties.  If so, look that address up as a link-routed
        // destination.
        //
        qd_field_iterator_t *dnp_address = qdr_terminus_dnp_address(terminus);
        if (dnp_address) {
            qd_address_iterator_override_prefix(dnp_address, qdr_prefix_for_dir(dir));
            qd_hash_retrieve_prefix(core->addr_hash, dnp_address, (void**) &addr);
            qd_field_iterator_free(dnp_address);
            *link_route = true;
            return addr;
        }

        //
        // The dynamic terminus has no address in the dynamic-node-propteries.  If we are
        // permitted to generate dynamic addresses, create a new address that is local to
        // this router and insert it into the address table with a hash index.
        //
        if (!accept_dynamic)
            return 0;

        char temp_addr[200];
        bool generating = true;
        while (generating) {
            //
            // The address-generation process is performed in a loop in case the generated
            // address collides with a previously generated address (this should be _highly_
            // unlikely).
            //
            qdr_generate_temp_addr(core, temp_addr, 200);
            qd_field_iterator_t *temp_iter = qd_address_iterator_string(temp_addr, ITER_VIEW_ADDRESS_HASH);
            qd_hash_retrieve(core->addr_hash, temp_iter, (void**) &addr);
            if (!addr) {
                addr = qdr_address(qdr_dynamic_semantics);
                qd_hash_insert(core->addr_hash, temp_iter, addr, &addr->hash_handle);
                DEQ_INSERT_TAIL(core->addrs, addr);
                generating = false;
            }
            qd_field_iterator_free(temp_iter);
        }
        return addr;
    }

    //
    // If the terminus is anonymous, there is no address to look up.
    //
    if (qdr_terminus_is_anonymous(terminus))
        return 0;

    //
    // The terminus has a non-dynamic address that we need to look up.  First, look for
    // a link-route destination for the address.
    //
    qd_field_iterator_t *iter = qdr_terminus_get_address(terminus);
    qd_address_iterator_override_prefix(iter, qdr_prefix_for_dir(dir));
    qd_hash_retrieve_prefix(core->addr_hash, iter, (void**) &addr);
    if (addr) {
        *link_route = true;
        return addr;
    }

    //
    // There was no match for a link-route destination, look for a message-route address.
    //
    qd_address_iterator_override_prefix(iter, '\0'); // Cancel previous override
    qd_hash_retrieve(core->addr_hash, iter, (void**) &addr);
    if (!addr && create_if_not_found) {
        qd_address_semantics_t sem = qdr_semantics_for_address(core, iter);
        addr = qdr_address(sem);
        qd_hash_insert(core->addr_hash, iter, addr, &addr->hash_handle);
        DEQ_INSERT_TAIL(core->addrs, addr);
    }

    return addr;
}


static void qdr_connection_opened_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t *conn = action->args.connection.conn;
    DEQ_ITEM_INIT(conn);
    DEQ_INSERT_TAIL(core->open_connections, conn);

    if (conn->role == QDR_ROLE_NORMAL) {
        //
        // No action needed for NORMAL connections
        //
        return;
    }

    if (conn->role == QDR_ROLE_INTER_ROUTER) {
        //
        // Assign a unique mask-bit to this connection as a reference to be used by
        // the router module
        //
        if (qd_bitmask_first_set(core->neighbor_free_mask, &conn->mask_bit))
            qd_bitmask_clear_bit(core->neighbor_free_mask, conn->mask_bit);
        else {
            qd_log(core->log, QD_LOG_CRITICAL, "Exceeded maximum inter-router connection count");
            return;
        }

        if (!conn->incoming) {
            //
            // The connector-side of inter-router connections is responsible for setting up the
            // inter-router links:  Two (in and out) for control, two for routed-message transfer.
            //
            (void) qdr_create_link_CT(core, conn, QD_LINK_CONTROL, QD_INCOMING, qdr_terminus_router_control(), 0);
            (void) qdr_create_link_CT(core, conn, QD_LINK_CONTROL, QD_OUTGOING, 0, qdr_terminus_router_control());
            (void) qdr_create_link_CT(core, conn, QD_LINK_ROUTER,  QD_INCOMING, qdr_terminus_router_data(), 0);
            (void) qdr_create_link_CT(core, conn, QD_LINK_ROUTER,  QD_OUTGOING, 0, qdr_terminus_router_data());
        }
    }

    //
    // If the role is ON_DEMAND:
    //    Activate waypoints associated with this connection
    //    Activate link-route destinations associated with this connection
    //
}


static void qdr_connection_closed_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t *conn = action->args.connection.conn;

    //
    // TODO - Deactivate waypoints and link-route destinations for this connection
    //

    //
    // TODO - Clean up links associated with this connection
    //        This involves the links and the dispositions of deliveries stored
    //        with the links.
    //

    //
    // Discard items on the work list
    //

    DEQ_REMOVE(core->open_connections, conn);
    sys_mutex_free(conn->work_lock);
    free_qdr_connection_t(conn);
}


static void qdr_link_first_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    qdr_connection_t  *conn   = action->args.connection.conn;
    qdr_link_t        *link   = action->args.connection.link;
    qd_direction_t     dir    = action->args.connection.dir;
    //qdr_terminus_t    *source = action->args.connection.source;
    qdr_terminus_t    *target = action->args.connection.target;

    //
    // Reject any attaches of inter-router links that arrive on connections that are not inter-router.
    //
    if ((link->link_type == QD_LINK_CONTROL || link->link_type == QD_LINK_ROUTER) && conn->role != QDR_ROLE_INTER_ROUTER)
        qdr_link_reject_CT(core, link, QDR_CONDITION_FORBIDDEN);

    //
    // Reject any waypoint links.  Waypoint links are always initiated by a router, not the remote container.
    //
    if (link->link_type == QD_LINK_WAYPOINT)
        qdr_link_reject_CT(core, link, QDR_CONDITION_FORBIDDEN);

    if (dir == QD_INCOMING) {
        //
        // Handle incoming link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            if (qdr_terminus_is_anonymous(target)) {
                link->addr = 0;
                qdr_link_accept_CT(core, link);
            } else {
                //
                // This link has a target address
                //
                bool           link_route = false;
                qdr_address_t *addr = qdr_lookup_terminus_address_CT(core, dir, target, false, false, &link_route);
                if (!addr)
                    //
                    // No route to this destination, reject the link
                    //
                    qdr_link_reject_CT(core, link, QDR_CONDITION_NO_ROUTE_TO_DESTINATION);

                else if (link_route)
                    //
                    // This is a link-routed destination, forward the attach to the next hop
                    //
                    qdr_forward_first_attach_CT(core, link, addr);

                else {
                    //
                    // Associate the link with the address.  With this association, it will be unnecessary
                    // to do an address lookup for deliveries that arrive on this link.
                    //
                    link->addr = addr;
                    qdr_link_accept_CT(core, link);
                }
            }
            break;

        case QD_LINK_WAYPOINT:
            // No action, waypoint links are rejected above.
            break;

        case QD_LINK_CONTROL:
            break;

        case QD_LINK_ROUTER:
            break;
        }
    } else {
        //
        // Handle outgoing link cases
        //
        switch (link->link_type) {
        case QD_LINK_ENDPOINT:
            break;
        case QD_LINK_WAYPOINT:
            // No action, waypoint links are rejected above.
            break;
        case QD_LINK_CONTROL:
            break;
        case QD_LINK_ROUTER:
            break;
        }
    }

    //
    // Cases to be handled:
    //
    // dir = Incoming or Outgoing:
    //    Link is an router-control link
    //       If this isn't an inter-router connection, close the link
    //       Note the control link on the connection
    //       Issue a second attach back to the originating node
    //    Link is addressed (i.e. has a target/source address)
    //       If this is a link-routed address, Issue a first attach to the next hop
    //       If not link-routed, issue a second attach back to the originating node
    //
    // dir = Incoming:
    //    Link is addressed (i.e. has a target address) and not link-routed
    //       Lookup/Create address in the address table and associate the link to the address
    //       Issue a second attach back to the originating node
    //    Link is anonymous
    //       Issue a second attach back to the originating node
    //    Issue credit for the inbound fifo
    //
    // dir = Outgoing:
    //    Link is a router-control link
    //       Associate the link with the router-hello address
    //       Associate the link with the link-mask-bit being used by the router
    //    Link is addressed (i.e. has a non-dynamic source address)
    //       If the address is appropriate for distribution, add it to the address table as a local destination
    //       If this is the first local dest for this address, notify the router (mobile_added)
    //       Issue a second attach back to the originating node
    //
}


static void qdr_link_second_attach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    //qdr_link_t     *link   = action->args.connection.link;
    //qdr_terminus_t *source = action->args.connection.source;
    //qdr_terminus_t *target = action->args.connection.target;

    //
    // Cases to be handled:
    //
    // Link is a router-control link:
    //    Note the control link on the connection
    //    Associate the link with the router-hello address
    //    Associate the link with the link-mask-bit being used by the router
    // Link is link-routed:
    //    Propagate the second attach back toward the originating node
    // Link is Incoming:
    //    Issue credit for the inbound fifo
    //
}


static void qdr_link_detach_CT(qdr_core_t *core, qdr_action_t *action, bool discard)
{
    if (discard)
        return;

    //qdr_link_t     *link      = action->args.connection.link;
    //pn_condition_t *condition = action->args.connection.condition;

    //
    // Cases to be handled:
    //
    // Link is link-routed:
    //    Propagate the detach along the link-chain
    // Link is half-detached and not link-routed:
    //    Issue a detach back to the originating node
    // Link is fully detached:
    //    Free the qdr_link object
    //    Remove any address linkages associated with this link
    //       If the last dest for a local address is lost, notify the router (mobile_removed)
    // Link is a router-control link:
    //    Issue a link-lost indication to the router
    //
}


