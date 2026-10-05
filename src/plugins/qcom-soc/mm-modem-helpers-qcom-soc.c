/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mm-modem-helpers-qcom-soc.h"

/* Select only when no primary GW session exists. The returned AID is borrowed
 * from cards; the provisioning request uses a one-based physical slot. */
gboolean
mm_qcom_soc_select_uim_application (guint16  index_gw_primary,
                                   GArray  *cards,
                                   guint8  *slot,
                                   GArray **aid)
{
    guint i;

    if ((index_gw_primary >> 8) != 0xff && (index_gw_primary & 0xff) != 0xff)
        return FALSE;

    if (!cards)
        return FALSE;

    for (i = 0; i < cards->len && i < G_MAXUINT8; i++) {
        QmiMessageUimGetCardStatusOutputCardStatusCardsElement *card;
        QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *selected = NULL;
        guint j;

        card = &g_array_index (cards, QmiMessageUimGetCardStatusOutputCardStatusCardsElement, i);
        if (card->card_state != QMI_UIM_CARD_STATE_PRESENT || !card->applications)
            continue;

        for (j = 0; j < card->applications->len; j++) {
            QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 *app;

            app = &g_array_index (card->applications, QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2, j);
            if (app->state == QMI_UIM_CARD_APPLICATION_STATE_ILLEGAL)
                continue;
            if (app->type == QMI_UIM_CARD_APPLICATION_TYPE_USIM) {
                selected = app;
                break;
            }
            if (app->type == QMI_UIM_CARD_APPLICATION_TYPE_SIM && !selected)
                selected = app;
        }

        if (selected) {
            *slot = i + 1;
            *aid = selected->application_identifier_value;
            return TRUE;
        }
    }

    return FALSE;
}
