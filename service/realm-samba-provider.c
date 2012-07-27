/* realmd -- Realm configuration service
 *
 * Copyright 2012 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#include "realm-ad-discover.h"
#include "realm-command.h"
#include "realm-daemon.h"
#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-discovery.h"
#include "realm-errors.h"
#include "realm-kerberos.h"
#include "realm-packages.h"
#include "realm-samba.h"
#include "realm-samba-config.h"
#include "realm-samba-enroll.h"
#include "realm-samba-provider.h"
#include "realm-samba-winbind.h"

#include <glib/gstdio.h>

#include <errno.h>

struct _RealmSambaProvider {
	RealmProvider parent;
	RealmIniConfig *config;
};

typedef struct {
	RealmProviderClass parent_class;
} RealmSambaProviderClass;

enum {
	PROP_0,
	PROP_SAMBA_CONFIG,
};

#define   REALM_DBUS_SAMBA_PATH                    "/org/freedesktop/realmd/Samba"

G_DEFINE_TYPE (RealmSambaProvider, realm_samba_provider, REALM_TYPE_PROVIDER);

static void
realm_samba_provider_init (RealmSambaProvider *self)
{
	self->config = realm_samba_config_new (NULL);

	/* The dbus Name property of the provider */
	g_object_set (self, "name", "Samba", NULL);
}

static void
realm_samba_provider_constructed (GObject *obj)
{
	RealmSambaProvider *self;
	gchar *name = NULL;
	gchar *security;

	G_OBJECT_CLASS (realm_samba_provider_parent_class)->constructed (obj);

	self = REALM_SAMBA_PROVIDER (obj);
	security = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "security");
	if (security != NULL && g_ascii_strcasecmp (security, "ADS") == 0)
		name = realm_ini_config_get (self->config, REALM_SAMBA_CONFIG_GLOBAL, "realm");

	if (name != NULL) {
		realm_provider_lookup_or_register_realm (REALM_PROVIDER (self),
		                                         REALM_TYPE_SAMBA, name);
	}

	g_free (name);
	g_free (security);
}

static void
on_ad_discover (GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
	GSimpleAsyncResult *async = G_SIMPLE_ASYNC_RESULT (user_data);
	g_simple_async_result_set_op_res_gpointer (async, g_object_ref (result), g_object_unref);
	g_simple_async_result_complete (async);
	g_object_unref (async);
}

static void
realm_samba_provider_discover_async (RealmProvider *provider,
                                     const gchar *string,
                                     GDBusMethodInvocation *invocation,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
	GSimpleAsyncResult *async;

	async = g_simple_async_result_new (G_OBJECT (provider), callback, user_data,
	                                   realm_samba_provider_discover_async);

	realm_ad_discover_async (string, invocation, on_ad_discover, g_object_ref (async));

	g_object_unref (async);
}

static gint
realm_samba_provider_discover_finish (RealmProvider *provider,
                                      GAsyncResult *result,
                                      GVariant **realms,
                                      GError **error)
{
	GDBusInterfaceSkeleton *realm;
	GSimpleAsyncResult *async;
	GHashTable *discovery;
	GAsyncResult *ad_result;
	const gchar *object_path;
	GVariant *realm_info;
	gchar *name;

	async = G_SIMPLE_ASYNC_RESULT (result);
	ad_result = g_simple_async_result_get_op_res_gpointer (async);

	name = realm_ad_discover_finish (ad_result, &discovery, error);
	if (name == NULL)
		return 0;

	realm = realm_provider_lookup_or_register_realm (provider,
	                                                 REALM_TYPE_SAMBA,
	                                                 name);
	g_free (name);

	if (realm == NULL) {
		g_hash_table_unref (discovery);
		return 0;
	}

	realm_kerberos_set_discovery (REALM_KERBEROS (realm), discovery);

	object_path = g_dbus_interface_skeleton_get_object_path (G_DBUS_INTERFACE_SKELETON (realm));
	realm_info = realm_provider_new_realm_info (object_path, REALM_DBUS_KERBEROS_REALM_INTERFACE);
	*realms = g_variant_new_array (G_VARIANT_TYPE ("(os)"), &realm_info, 1);
	g_variant_ref_sink (*realms);

	g_hash_table_unref (discovery);

	/* Return a higher priority if we're the default */
	return realm_provider_is_default ("active-directory", "winbind") ? 100 : 50;
}

static void
realm_samba_provider_get_property (GObject *obj,
                                   guint prop_id,
                                   GValue *value,
                                   GParamSpec *pspec)
{
	RealmSambaProvider *self = REALM_SAMBA_PROVIDER (obj);

	switch (prop_id) {
	case PROP_SAMBA_CONFIG:
		g_value_set_object (value, self->config);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_samba_provider_finalize (GObject *obj)
{
	RealmSambaProvider *self = REALM_SAMBA_PROVIDER (obj);

	g_object_unref (self->config);

	G_OBJECT_CLASS (realm_samba_provider_parent_class)->finalize (obj);
}

void
realm_samba_provider_class_init (RealmSambaProviderClass *klass)
{
	RealmProviderClass *provider_class = REALM_PROVIDER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	provider_class->dbus_path = REALM_DBUS_SAMBA_PATH;

	provider_class->discover_async = realm_samba_provider_discover_async;
	provider_class->discover_finish = realm_samba_provider_discover_finish;

	object_class->constructed = realm_samba_provider_constructed;
	object_class->get_property = realm_samba_provider_get_property;
	object_class->finalize = realm_samba_provider_finalize;

	g_object_class_install_property (object_class, PROP_SAMBA_CONFIG,
	            g_param_spec_object ("samba-config", "Samba Config", "Samba Config",
	                                 REALM_TYPE_INI_CONFIG, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

}
