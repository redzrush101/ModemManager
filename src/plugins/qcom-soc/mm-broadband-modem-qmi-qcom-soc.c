/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2020 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log.h"
#include "mm-iface-modem.h"
#include "mm-shared-qmi.h"
#include "mm-modem-helpers-qcom-soc.h"
#include "mm-broadband-modem-qmi-qcom-soc.h"

static void iface_modem_init (MMIfaceModemInterface *iface);
static MMIfaceModemInterface *iface_modem_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQmiQcomSoc, mm_broadband_modem_qmi_qcom_soc, MM_TYPE_BROADBAND_MODEM_QMI, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init))

/*****************************************************************************/
/* Select a primary GW SIM application before querying its lock state. */

static MMModemLock
load_unlock_required_finish (MMIfaceModem  *self,
                             GAsyncResult  *res,
                             GError       **error)
{
    gssize value;

    value = g_task_propagate_int (G_TASK (res), error);
    return value < 0 ? MM_MODEM_LOCK_UNKNOWN : (MMModemLock) value;
}

static void
parent_load_unlock_required_ready (MMIfaceModem *self,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    MMModemLock lock;
    GError *error = NULL;

    lock = iface_modem_parent->load_unlock_required_finish (self, res, &error);
    if (error)
        g_task_return_error (task, error);
    else
        g_task_return_int (task, lock);
    g_object_unref (task);
}

static void
parent_load_unlock_required (GTask *task)
{
    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    iface_modem_parent->load_unlock_required (g_task_get_source_object (task),
                                            GPOINTER_TO_INT (g_task_get_task_data (task)),
                                            g_task_get_cancellable (task),
                                            (GAsyncReadyCallback) parent_load_unlock_required_ready,
                                            task);
}

static void
change_provisioning_session_ready (QmiClientUim *client,
                                   GAsyncResult *res,
                                   GTask        *task)
{
    g_autoptr(QmiMessageUimChangeProvisioningSessionOutput) output = NULL;
    GError *error = NULL;

    output = qmi_client_uim_change_provisioning_session_finish (client, res, &error);
    if (!output || !qmi_message_uim_change_provisioning_session_output_get_result (output, &error)) {
        g_prefix_error (&error, "Couldn't activate primary GW SIM application: ");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    parent_load_unlock_required (task);
}

static void
get_card_status_ready (QmiClientUim *client,
                       GAsyncResult *res,
                       GTask        *task)
{
    g_autoptr(QmiMessageUimGetCardStatusOutput) output = NULL;
    g_autoptr(QmiMessageUimChangeProvisioningSessionInput) input = NULL;
    GError *error = NULL;
    guint16 index_gw_primary;
    GArray *cards;
    GArray *aid;
    guint8 slot;

    output = qmi_client_uim_get_card_status_finish (client, res, &error);
    if (!output || !qmi_message_uim_get_card_status_output_get_result (output, &error) ||
        !qmi_message_uim_get_card_status_output_get_card_status (output, &index_gw_primary,
                                                              NULL, NULL, NULL, &cards, &error)) {
        /* Older firmware may support only DMS UIM commands. Let the parent
         * handle that case, but always honor cancellation. */
        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_task_return_error (task, error);
            g_object_unref (task);
            return;
        }
        mm_obj_dbg (g_task_get_source_object (task), "couldn't check primary GW session: %s", error->message);
        g_clear_error (&error);
        parent_load_unlock_required (task);
        return;
    }

    if (!mm_qcom_soc_select_uim_application (index_gw_primary, cards, &slot, &aid)) {
        parent_load_unlock_required (task);
        return;
    }

    if (g_task_return_error_if_cancelled (task)) {
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (g_task_get_source_object (task), "activating primary GW SIM application on slot %u", slot);
    input = qmi_message_uim_change_provisioning_session_input_new ();
    qmi_message_uim_change_provisioning_session_input_set_session_change (
        input, QMI_UIM_SESSION_TYPE_PRIMARY_GW_PROVISIONING, TRUE, NULL);
    qmi_message_uim_change_provisioning_session_input_set_application_information (input, slot, aid, NULL);
    qmi_client_uim_change_provisioning_session (client, input, 10,
                                              g_task_get_cancellable (task),
                                              (GAsyncReadyCallback) change_provisioning_session_ready,
                                              task);
}

static void
load_unlock_required (MMIfaceModem        *self,
                      gboolean             last_attempt,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
    QmiClient *client;
    GTask *task;

    task = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (task, GINT_TO_POINTER (last_attempt), NULL);

    client = mm_shared_qmi_peek_client (MM_SHARED_QMI (self), QMI_SERVICE_UIM,
                                      MM_PORT_QMI_FLAG_DEFAULT, NULL);
    if (!client) {
        parent_load_unlock_required (task);
        return;
    }

    qmi_client_uim_get_card_status (QMI_CLIENT_UIM (client), NULL, 5, cancellable,
                                  (GAsyncReadyCallback) get_card_status_ready, task);
}

/*****************************************************************************/

static const QmiSioPort sio_port_per_port_number[] = {
    QMI_SIO_PORT_A2_MUX_RMNET0,
    QMI_SIO_PORT_A2_MUX_RMNET1,
    QMI_SIO_PORT_A2_MUX_RMNET2,
    QMI_SIO_PORT_A2_MUX_RMNET3,
    QMI_SIO_PORT_A2_MUX_RMNET4,
    QMI_SIO_PORT_A2_MUX_RMNET5,
    QMI_SIO_PORT_A2_MUX_RMNET6,
    QMI_SIO_PORT_A2_MUX_RMNET7
};

static MMPortQmi *
peek_port_qmi_for_data_bam_dmux (MMBroadbandModemQmi  *self,
                                 MMPort               *data,
                                 MMQmiDataEndpoint    *out_endpoint,
                                 GError              **error)
{
    MMPortQmi      *found = NULL;
    MMKernelDevice *net_port;
    gint            net_port_number;

    net_port = mm_port_peek_kernel_device (data);

    /* The dev_port notified by the bam-dmux driver indicates which SIO port we should be using */
    net_port_number = mm_kernel_device_get_attribute_as_int (net_port, "dev_port");
    if (net_port_number < 0 || net_port_number >= (gint) G_N_ELEMENTS (sio_port_per_port_number)) {
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find SIO port number for 'net/%s'",
                     mm_port_get_device (data));
        return NULL;
    }

    /* Find one QMI port, we don't care which one */
    found = mm_broadband_modem_qmi_peek_port_qmi (self);

    if (!found)
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find any QMI port for 'net/%s'",
                     mm_port_get_device (data));
    else if (out_endpoint) {
        /* WDS Bind (Mux) Data Port must be called with the correct endpoint
         * interface number/SIO port to make multiplexing work with BAM-DMUX */
        out_endpoint->type = QMI_DATA_ENDPOINT_TYPE_BAM_DMUX;
        out_endpoint->interface_number = net_port_number;
        out_endpoint->sio_port = sio_port_per_port_number[net_port_number];
    }

    return found;
}

static MMPortQmi *
peek_port_qmi_for_data_ipa (MMBroadbandModemQmi  *self,
                            MMPort               *data,
                            MMQmiDataEndpoint    *out_endpoint,
                            GError              **error)
{
    MMPortQmi *found = NULL;

    /* when using IPA, we have a main network interface that will be multiplexed
     * to create link interfaces. We can assume any of the available QMI ports is
     * able to manage that. */

    found = mm_broadband_modem_qmi_peek_port_qmi (self);

    if (!found)
        g_set_error (error,
                     MM_CORE_ERROR,
                     MM_CORE_ERROR_NOT_FOUND,
                     "Couldn't find any QMI port for 'net/%s'",
                     mm_port_get_device (data));
    else if (out_endpoint)
        mm_port_qmi_get_endpoint_info (found, out_endpoint);

    return found;
}

static MMPortQmi *
peek_port_qmi_for_data (MMBroadbandModemQmi  *self,
                        MMPort               *data,
                        MMQmiDataEndpoint    *out_endpoint,
                        GError              **error)
{
    MMKernelDevice *net_port;
    const gchar    *net_port_driver;

    g_assert (MM_IS_BROADBAND_MODEM_QMI (self));
    g_assert (mm_port_get_subsys (data) == MM_PORT_SUBSYS_NET);

    net_port = mm_port_peek_kernel_device (data);
    net_port_driver = mm_kernel_device_get_driver (net_port);

    if (g_strcmp0 (net_port_driver, "ipa") == 0)
        return peek_port_qmi_for_data_ipa (self, data, out_endpoint, error);

    if (g_strcmp0 (net_port_driver, "bam-dmux") == 0)
        return peek_port_qmi_for_data_bam_dmux (self, data, out_endpoint, error);

    g_set_error (error,
                 MM_CORE_ERROR,
                 MM_CORE_ERROR_FAILED,
                 "Unsupported QMI kernel driver for 'net/%s': %s",
                 mm_port_get_device (data),
                 net_port_driver);
    return NULL;
}

/*****************************************************************************/

MMBroadbandModemQmiQcomSoc *
mm_broadband_modem_qmi_qcom_soc_new (const gchar  *device,
                                     const gchar  *physdev,
                                     const gchar **drivers,
                                     const gchar  *plugin,
                                     guint16       vendor_id,
                                     guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI_QCOM_SOC,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_PHYSDEV,    physdev,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* QMI bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         NULL);
}

static void
mm_broadband_modem_qmi_qcom_soc_init (MMBroadbandModemQmiQcomSoc *self)
{
}

static void
mm_broadband_modem_qmi_qcom_soc_class_init (MMBroadbandModemQmiQcomSocClass *klass)
{
    MMBroadbandModemQmiClass *broadband_modem_qmi_class = MM_BROADBAND_MODEM_QMI_CLASS (klass);

    broadband_modem_qmi_class->peek_port_qmi_for_data = peek_port_qmi_for_data;
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);
    iface->load_unlock_required = load_unlock_required;
    iface->load_unlock_required_finish = load_unlock_required_finish;
}
