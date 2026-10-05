/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "mm-modem-helpers-qcom-soc.h"

typedef QmiMessageUimGetCardStatusOutputCardStatusCardsElement Card;
typedef QmiMessageUimGetCardStatusOutputCardStatusCardsElementApplicationsElementV2 Application;

static void
card_clear (gpointer data)
{
    Card *card = data;
    guint i;

    for (i = 0; i < card->applications->len; i++) {
        Application *app = &g_array_index (card->applications, Application, i);

        g_array_unref (app->application_identifier_value);
    }
    g_array_unref (card->applications);
}

static GArray *
cards_new (void)
{
    GArray *cards;
    guint i;

    cards = g_array_new (FALSE, FALSE, sizeof (Card));
    g_array_set_clear_func (cards, card_clear);
    for (i = 0; i < 2; i++) {
        Card card = { 0 };

        card.card_state = QMI_UIM_CARD_STATE_ABSENT;
        card.applications = g_array_new (FALSE, FALSE, sizeof (Application));
        g_array_append_val (cards, card);
    }
    return cards;
}

static void
add_application (GArray                     *cards,
                 guint                       slot_i,
                 QmiUimCardApplicationType   type,
                 QmiUimCardApplicationState  state,
                 guint8                      aid_byte)
{
    Card *card = &g_array_index (cards, Card, slot_i);
    Application app = { 0 };

    app.type = type;
    app.state = state;
    app.application_identifier_value = g_array_new (FALSE, FALSE, sizeof (guint8));
    g_array_append_val (app.application_identifier_value, aid_byte);
    g_array_append_val (card->applications, app);
}

static void
test_slot_two (void)
{
    g_autoptr(GArray) cards = cards_new ();
    guint8 slot = 0;
    GArray *aid = NULL;

    /* An absent card may still report applications. Never borrow its AID. */
    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x11);
    g_array_index (cards, Card, 1).card_state = QMI_UIM_CARD_STATE_PRESENT;
    add_application (cards, 1, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_PIN1_OR_UPIN_PIN_REQUIRED, 0x22);

    g_assert_true (mm_qcom_soc_select_uim_application (0xffff, cards, &slot, &aid));
    g_assert_cmpuint (slot, ==, 2);
    g_assert_cmpuint (g_array_index (aid, guint8, 0), ==, 0x22);
}

static void
test_preserve_selection (void)
{
    g_autoptr(GArray) cards = cards_new ();
    guint8 slot = 0;
    GArray *aid = NULL;

    g_array_index (cards, Card, 0).card_state = QMI_UIM_CARD_STATE_PRESENT;
    g_array_index (cards, Card, 1).card_state = QMI_UIM_CARD_STATE_PRESENT;
    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x11);
    add_application (cards, 1, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x22);

    g_assert_false (mm_qcom_soc_select_uim_application (0x0100, cards, &slot, &aid));
    g_assert_cmpuint (slot, ==, 0);
    g_assert_null (aid);
}

static void
test_no_usable_application (void)
{
    g_autoptr(GArray) cards = cards_new ();
    guint8 slot = 0;
    GArray *aid = NULL;

    g_assert_false (mm_qcom_soc_select_uim_application (0xffff, cards, &slot, &aid));
    g_array_index (cards, Card, 0).card_state = QMI_UIM_CARD_STATE_PRESENT;
    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_ILLEGAL, 0x11);
    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_ISIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x12);
    g_assert_false (mm_qcom_soc_select_uim_application (0xffff, cards, &slot, &aid));
}

static void
test_application_preference (void)
{
    g_autoptr(GArray) cards = cards_new ();
    guint8 slot = 0;
    GArray *aid = NULL;

    g_array_index (cards, Card, 0).card_state = QMI_UIM_CARD_STATE_PRESENT;
    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_SIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x11);
    g_assert_true (mm_qcom_soc_select_uim_application (0xffff, cards, &slot, &aid));
    g_assert_cmpuint (slot, ==, 1);
    g_assert_cmpuint (g_array_index (aid, guint8, 0), ==, 0x11);

    add_application (cards, 0, QMI_UIM_CARD_APPLICATION_TYPE_USIM, QMI_UIM_CARD_APPLICATION_STATE_READY, 0x12);
    g_assert_true (mm_qcom_soc_select_uim_application (0xffff, cards, &slot, &aid));
    g_assert_cmpuint (g_array_index (aid, guint8, 0), ==, 0x12);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/qcom-soc/uim/slot-two", test_slot_two);
    g_test_add_func ("/qcom-soc/uim/preserve-selection", test_preserve_selection);
    g_test_add_func ("/qcom-soc/uim/no-usable-application", test_no_usable_application);
    g_test_add_func ("/qcom-soc/uim/application-preference", test_application_preference);
    return g_test_run ();
}
