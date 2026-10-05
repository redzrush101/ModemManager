/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef MM_MODEM_HELPERS_QCOM_SOC_H
#define MM_MODEM_HELPERS_QCOM_SOC_H

#include <libqmi-glib.h>

gboolean mm_qcom_soc_select_uim_application (guint16  index_gw_primary,
                                            GArray  *cards,
                                            guint8  *slot,
                                            GArray **aid);

#endif
