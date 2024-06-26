#pragma once

#include <glib-object.h>
#include <gtkwave.h>

G_BEGIN_DECLS

#define GW_TYPE_VCD_LOADER (gw_vcd_loader_get_type())
G_DECLARE_FINAL_TYPE(GwVcdLoader, gw_vcd_loader, GW, VCD_LOADER, GwLoader)

GwLoader *gw_vcd_loader_new(void);

void gw_vcd_loader_set_vlist_prepack(GwVcdLoader *self, gboolean vlist_prepack);
gboolean gw_vcd_loader_is_vlist_prepack(GwVcdLoader *self);
void gw_vcd_loader_set_vlist_compression_level(GwVcdLoader *self, gint level);
gint gw_vcd_loader_get_vlist_compression_level(GwVcdLoader *self);
void gw_vcd_loader_set_warning_filesize(GwVcdLoader *self, guint warning_filesize);
guint gw_vcd_loader_get_warning_filesize(GwVcdLoader *self);

G_END_DECLS
