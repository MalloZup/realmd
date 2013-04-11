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

#include "realm-command.h"
#include "realm-credential.h"
#include "realm-daemon.h"
#include "realm-dbus-constants.h"
#include "realm-dbus-generated.h"
#include "realm-diagnostics.h"
#include "realm-discovery.h"
#include "realm-errors.h"
#include "realm-invocation.h"
#include "realm-kerberos.h"
#include "realm-kerberos-membership.h"
#include "realm-login-name.h"
#include "realm-packages.h"
#include "realm-provider.h"
#include "realm-settings.h"

#include <krb5/krb5.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

struct _RealmKerberosPrivate {
	GHashTable *discovery;
	RealmDbusRealm *realm_iface;
	RealmDbusKerberos *kerberos_iface;
	RealmDbusKerberosMembership *membership_iface;
};

enum {
	PROP_0,
	PROP_NAME,
	PROP_DISCOVERY,
	PROP_PROVIDER,
};

G_DEFINE_TYPE (RealmKerberos, realm_kerberos, G_TYPE_DBUS_OBJECT_SKELETON);

#define return_if_krb5_failed(ctx, code) G_STMT_START \
	if G_LIKELY ((code) == 0) { } else { \
		g_warn_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
		                krb5_get_error_message ((ctx), (code))); \
		 return; \
	} G_STMT_END

#define return_val_if_krb5_failed(ctx, code, val) G_STMT_START \
	if G_LIKELY ((code) == 0) { } else { \
		g_warn_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
		                krb5_get_error_message ((ctx), (code))); \
		 return (val); \
	} G_STMT_END

#define warn_if_krb5_failed(ctx, code) G_STMT_START \
	if G_LIKELY ((code) == 0) { } else { \
		g_warn_message (G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC, \
		                krb5_get_error_message ((ctx), (code))); \
	} G_STMT_END

typedef struct {
	RealmKerberos *self;
	GDBusMethodInvocation *invocation;
	RealmCredential *cred;
} MethodClosure;

static MethodClosure *
method_closure_new (RealmKerberos *self,
                    GDBusMethodInvocation *invocation)
{
	MethodClosure *method = g_slice_new0 (MethodClosure);
	method->self = g_object_ref (self);
	method->invocation = g_object_ref (invocation);
	return method;
}

static void
method_closure_free (MethodClosure *closure)
{
	g_object_unref (closure->self);
	g_object_unref (closure->invocation);
	if (closure->cred)
		realm_credential_unref (closure->cred);
	g_slice_free (MethodClosure, closure);
}

static void
enroll_method_reply (GDBusMethodInvocation *invocation,
                     GError *error)
{
	if (error == NULL) {
		realm_diagnostics_info (invocation, "Successfully enrolled machine in realm");
		g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

	} else if (error->domain == REALM_ERROR || error->domain == G_DBUS_ERROR) {
		realm_diagnostics_error (invocation, error, NULL);
		g_dbus_method_invocation_return_gerror (invocation, error);

	} else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		realm_diagnostics_error (invocation, error, "Cancelled");
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_CANCELLED,
		                                       _("Operation was cancelled."));

	} else {
		realm_diagnostics_error (invocation, error, "Failed to enroll machine in realm");
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_FAILED,
		                                       _("Failed to enroll machine in realm. See diagnostics."));
	}

	realm_invocation_unlock_daemon (invocation);
}

static void
on_name_caches_flush (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
	MethodClosure *closure = user_data;
	GError *error = NULL;
	gint status;

	status = realm_command_run_finish (result, NULL, &error);
	if (status != 0) {
		realm_diagnostics_error (closure->invocation, error,
		                         "Flushing name caches failed");
	}

	g_clear_error (&error);
	enroll_method_reply (closure->invocation, NULL);
	method_closure_free (closure);
}

static void
on_enroll_complete (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	MethodClosure *closure = user_data;
	RealmKerberosMembershipIface *iface;
	GCancellable *cancellable;
	GError *error = NULL;

	iface = REALM_KERBEROS_MEMBERSHIP_GET_IFACE (closure->self);
	g_return_if_fail (iface->join_finish != NULL);

	cancellable = realm_invocation_get_cancellable (closure->invocation);
	if (!g_cancellable_set_error_if_cancelled (cancellable, &error))
		(iface->join_finish) (REALM_KERBEROS_MEMBERSHIP (closure->self), result, &error);

	if (error != NULL) {
		enroll_method_reply (closure->invocation, error);
		method_closure_free (closure);
		g_clear_error (&error);

	/* Only flush the name caches if not in install mode */
	} else if (!realm_daemon_is_install_mode ()) {
		realm_command_run_known_async ("name-caches-flush", NULL, closure->invocation,
		                               on_name_caches_flush, closure);

	} else {
		enroll_method_reply (closure->invocation, NULL);
	}
}

static void
unenroll_method_reply (GDBusMethodInvocation *invocation,
                       GError *error)
{
	if (error == NULL) {
		realm_diagnostics_info (invocation, "Successfully unenrolled machine from realm");
		g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

	} else if (error->domain == REALM_ERROR || error->domain == G_DBUS_ERROR) {
		realm_diagnostics_error (invocation, error, NULL);
		g_dbus_method_invocation_return_gerror (invocation, error);

	} else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		realm_diagnostics_error (invocation, error, "Cancelled");
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_CANCELLED,
		                                       _("Operation was cancelled."));

	} else {
		realm_diagnostics_error (invocation, error, "Failed to unenroll machine from realm");
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_FAILED,
		                                       _("Failed to unenroll machine from domain. See diagnostics."));
	}

	realm_invocation_unlock_daemon (invocation);
}

static void
on_unenroll_complete (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
	MethodClosure *closure = user_data;
	RealmKerberosMembershipIface *iface;
	GCancellable *cancellable;
	GError *error = NULL;

	iface = REALM_KERBEROS_MEMBERSHIP_GET_IFACE (closure->self);
	g_return_if_fail (iface->leave_finish != NULL);

	cancellable = realm_invocation_get_cancellable (closure->invocation);
	if (!g_cancellable_set_error_if_cancelled (cancellable, &error))
		(iface->leave_finish) (REALM_KERBEROS_MEMBERSHIP (closure->self), result, &error);

	unenroll_method_reply (closure->invocation, error);

	g_clear_error (&error);
	method_closure_free (closure);
}

static gboolean
is_credential_supported (RealmKerberosMembershipIface *iface,
                         RealmCredential *cred,
                         gboolean join,
                         GError **error)
{
	const RealmCredential *supported;
	const char *message;
	gboolean found = FALSE;
	gint i;

	supported = join ? iface->join_creds_supported : iface->leave_creds_supported;
	if (supported) {
		for (i = 0; supported[i].type != 0; i++) {
			if (cred->type == supported[i].type) {
				found = TRUE;
				break;
			}
		}
	}

	if (found)
		return TRUE;

	switch (cred->type) {
	case REALM_CREDENTIAL_AUTOMATIC:
		message = join ? _("Joining this realm without credentials is not supported") :
		                 _("Leaving this realm without credentials is not supported");
		break;
	case REALM_CREDENTIAL_CCACHE:
		message = join ? _("Joining this realm using a credential cache is not supported") :
		                 _("Leaving this realm using a credential cache is not supported");
		break;
	case REALM_CREDENTIAL_SECRET:
		message = join ? _("Joining this realm using a secret is not supported") :
		                 _("Unenrolling this realm using a secret is not supported");
		break;
	case REALM_CREDENTIAL_PASSWORD:
		message = join ? _("Enrolling this realm using a password is not supported") :
		                 _("Unenrolling this realm using a password is not supported");
		break;
	}

	g_set_error_literal (error, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, message);
	return FALSE;
}

static void
join_or_leave (RealmKerberos *self,
               GVariant *credential,
               RealmKerberosFlags flags,
               GVariant *options,
               GDBusMethodInvocation *invocation,
               gboolean join)
{
	RealmKerberosMembershipIface *iface = REALM_KERBEROS_MEMBERSHIP_GET_IFACE (self);
	RealmCredential *cred;
	MethodClosure *method;
	GError *error = NULL;

	if ((join && iface && iface->join_async == NULL) ||
	    (!join && iface && iface->leave_async == NULL)) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
		                                       join ? _("Joining this realm is not supported") :
		                                              _("Leaving this realm is not supported"));
		return;
	}

	cred = realm_credential_parse (credential, &error);
	if (error != NULL) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		g_error_free (error);
		return;
	}

	if (!is_credential_supported (iface, cred, join, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		realm_credential_unref (cred);
		g_error_free (error);
		return;
	}

	if (!realm_invocation_lock_daemon (invocation)) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       _("Already running another action"));
		return;
	}

	method = method_closure_new (self, invocation);
	method->cred = cred;

	if (join) {
		g_return_if_fail (iface->join_finish != NULL);
		(iface->join_async) (REALM_KERBEROS_MEMBERSHIP (self), cred, flags,
		                     options, invocation, on_enroll_complete, method);
	} else {
		g_return_if_fail (iface->leave_finish != NULL);
		(iface->leave_async) (REALM_KERBEROS_MEMBERSHIP (self), cred, flags,
		                      options, invocation, on_unenroll_complete, method);
	}
}

static gboolean
handle_join (RealmDbusKerberosMembership *membership,
             GDBusMethodInvocation *invocation,
             GVariant *credentials,
             GVariant *options,
             gpointer user_data)
{
	RealmKerberos *self = REALM_KERBEROS (user_data);
	gchar hostname[HOST_NAME_MAX + 1];
	RealmKerberosFlags flags = 0;
	gboolean assume = FALSE;
	gint ret;

	/* Check the host name */
	ret = gethostname (hostname, sizeof (hostname));
	if (ret < 0 || g_ascii_strcasecmp (hostname, "localhost") == 0 ||
	    g_ascii_strncasecmp (hostname, "localhost.", 10) == 0) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_FAILED,
		                                       "This computer's host name is not set correctly.");
		return TRUE;
	}

	if (g_variant_lookup (options, REALM_DBUS_OPTION_ASSUME_PACKAGES, "b", &assume) && assume)
		flags |= REALM_KERBEROS_ASSUME_PACKAGES;

	join_or_leave (self, credentials, flags, options, invocation, TRUE);
	return TRUE;
}

static gboolean
handle_leave (RealmDbusKerberosMembership *membership,
              GDBusMethodInvocation *invocation,
              GVariant *credentials,
              GVariant *options,
              gpointer user_data)
{
	RealmKerberos *self = REALM_KERBEROS (user_data);
	RealmKerberosFlags flags = 0;
	const gchar *computer_ou;

	if (g_variant_lookup (options, REALM_DBUS_OPTION_COMPUTER_OU, "&s", &computer_ou)) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		                                       "The computer-ou argument is not supported when leaving a domain.");
		return TRUE;
	}

	join_or_leave (self, credentials, flags, options, invocation, FALSE);
	return TRUE;
}

static gboolean
handle_deconfigure (RealmDbusRealm *realm,
                    GDBusMethodInvocation *invocation,
                    GVariant *options,
                    gpointer user_data)
{
	GVariant *credential;

	credential = g_variant_new ("(ss@v)", "automatic", "none",
	                            g_variant_new_variant (g_variant_new_string ("")));
	join_or_leave (REALM_KERBEROS (user_data), credential, 0, options, invocation, FALSE);
	g_variant_unref (credential);

	return TRUE;
}


static void
on_logins_complete (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	MethodClosure *closure = user_data;
	RealmKerberosClass *klass;
	GError *error = NULL;

	klass = REALM_KERBEROS_GET_CLASS (closure->self);
	g_return_if_fail (klass->logins_finish != NULL);

	if ((klass->logins_finish) (closure->self, result, &error)) {
		realm_diagnostics_info (closure->invocation, "Successfully changed permitted logins for realm");
		g_dbus_method_invocation_return_value (closure->invocation, g_variant_new ("()"));

	} else if (error != NULL &&
	           (error->domain == REALM_ERROR || error->domain == G_DBUS_ERROR)) {
		realm_diagnostics_error (closure->invocation, error, NULL);
		g_dbus_method_invocation_return_gerror (closure->invocation, error);
		g_error_free (error);

	} else {
		realm_diagnostics_error (closure->invocation, error, "Failed to change permitted logins");
		g_dbus_method_invocation_return_error (closure->invocation, REALM_ERROR, REALM_ERROR_INTERNAL,
		                                       _("Failed to change permitted logins. See diagnostics."));
		g_error_free (error);
	}

	realm_invocation_unlock_daemon (closure->invocation);
	method_closure_free (closure);
}

static gboolean
handle_change_login_policy (RealmDbusRealm *realm,
                            GDBusMethodInvocation *invocation,
                            const gchar *login_policy,
                            const gchar *const *add,
                            const gchar *const *remove,
                            GVariant *options,
                            gpointer user_data)
{
	RealmKerberosLoginPolicy policy = REALM_KERBEROS_POLICY_NOT_SET;
	RealmKerberos *self = REALM_KERBEROS (user_data);
	RealmKerberosClass *klass;
	gchar **policies;
	gint policies_set = 0;
	gint i;

	policies = g_strsplit_set (login_policy, ", \t", -1);
	for (i = 0; policies[i] != NULL; i++) {
		if (g_str_equal (policies[i], REALM_DBUS_LOGIN_POLICY_ANY)) {
			policy = REALM_KERBEROS_ALLOW_ANY_LOGIN;
			policies_set++;
		} else if (g_str_equal (policies[i], REALM_DBUS_LOGIN_POLICY_REALM)) {
			policy = REALM_KERBEROS_ALLOW_REALM_LOGINS;
			policies_set++;
		} else if (g_str_equal (policies[i], REALM_DBUS_LOGIN_POLICY_PERMITTED)) {
			policy = REALM_KERBEROS_ALLOW_PERMITTED_LOGINS;
			policies_set++;
		} else if (g_str_equal (policies[i], REALM_DBUS_LOGIN_POLICY_DENY)) {
			policy = REALM_KERBEROS_DENY_ANY_LOGIN;
			policies_set++;
		} else {
			g_strfreev (policies);
			g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
			                                       G_DBUS_ERROR_INVALID_ARGS,
			                                       "Invalid or unknown login_policy argument");
			return TRUE;
		}
	}

	g_strfreev (policies);

	if (policies_set > 1) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
		                                       G_DBUS_ERROR_INVALID_ARGS,
		                                       "Conflicting flags in login_policy argument");
		return TRUE;
	}

	if (!realm_invocation_lock_daemon (invocation)) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       _("Already running another action"));
		return TRUE;
	}

	klass = REALM_KERBEROS_GET_CLASS (self);
	g_return_val_if_fail (klass->logins_async != NULL, FALSE);

	(klass->logins_async) (self, invocation, policy, (const gchar **)add,
	                       (const gchar **)remove, on_logins_complete,
	                       method_closure_new (self, invocation));

	return TRUE;
}

static gboolean
realm_kerberos_authorize_method (GDBusObjectSkeleton    *object,
                                 GDBusInterfaceSkeleton *iface,
                                 GDBusMethodInvocation  *invocation)
{
	return realm_invocation_authorize (invocation);
}

static void
realm_kerberos_init (RealmKerberos *self)
{
	GDBusObjectSkeleton *skeleton = G_DBUS_OBJECT_SKELETON (self);

	self->pv = G_TYPE_INSTANCE_GET_PRIVATE (self, REALM_TYPE_KERBEROS,
	                                        RealmKerberosPrivate);

	self->pv->realm_iface = realm_dbus_realm_skeleton_new ();
	g_signal_connect (self->pv->realm_iface, "handle-deconfigure",
	                  G_CALLBACK (handle_deconfigure), self);
	g_signal_connect (self->pv->realm_iface, "handle-change-login-policy",
	                  G_CALLBACK (handle_change_login_policy), self);
	g_dbus_object_skeleton_add_interface (skeleton, G_DBUS_INTERFACE_SKELETON (self->pv->realm_iface));

	self->pv->kerberos_iface = realm_dbus_kerberos_skeleton_new ();
	g_dbus_object_skeleton_add_interface (skeleton, G_DBUS_INTERFACE_SKELETON (self->pv->kerberos_iface));
}

static void
realm_kerberos_constructed (GObject *obj)
{
	RealmKerberosMembershipIface *iface;
	RealmKerberos *self = REALM_KERBEROS (obj);
	const gchar *supported_interfaces[3];
	GVariant *supported;
	const gchar *name;

	G_OBJECT_CLASS (realm_kerberos_parent_class)->constructed (obj);

	if (REALM_IS_KERBEROS_MEMBERSHIP (self)) {
		self->pv->membership_iface = realm_dbus_kerberos_membership_skeleton_new ();
		g_signal_connect (self->pv->membership_iface, "handle-join",
		                  G_CALLBACK (handle_join), self);
		g_signal_connect (self->pv->membership_iface, "handle-leave",
		                  G_CALLBACK (handle_leave), self);
		g_dbus_object_skeleton_add_interface (G_DBUS_OBJECT_SKELETON (self),
		                                      G_DBUS_INTERFACE_SKELETON (self->pv->membership_iface));

		iface = REALM_KERBEROS_MEMBERSHIP_GET_IFACE (self);
		supported = realm_credential_build_supported (iface->join_creds_supported);
		realm_dbus_kerberos_membership_set_supported_join_credentials (self->pv->membership_iface, supported);

		iface = REALM_KERBEROS_MEMBERSHIP_GET_IFACE (self);
		supported = realm_credential_build_supported (iface->leave_creds_supported);
		realm_dbus_kerberos_membership_set_supported_leave_credentials (self->pv->membership_iface, supported);
	}

	supported_interfaces[0] = REALM_DBUS_KERBEROS_INTERFACE;
	if (self->pv->membership_iface)
		supported_interfaces[1] = REALM_DBUS_KERBEROS_MEMBERSHIP_INTERFACE;
	else
		supported_interfaces[1] = NULL;
	supported_interfaces[2] = NULL;

	realm_dbus_realm_set_supported_interfaces (self->pv->realm_iface,
	                                           supported_interfaces);

	if (self->pv->discovery) {
		name = realm_discovery_get_string (self->pv->discovery, REALM_DBUS_DISCOVERY_DOMAIN);
		if (name)
			realm_kerberos_set_domain_name (self, name);
		name = realm_discovery_get_string (self->pv->discovery, REALM_DBUS_DISCOVERY_REALM);
		if (name)
			realm_kerberos_set_realm_name (self, name);
	}
}

static void
realm_kerberos_get_property (GObject *obj,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
	RealmKerberos *self = REALM_KERBEROS (obj);

	switch (prop_id) {
	case PROP_NAME:
		g_value_set_string (value, realm_kerberos_get_name (self));
		break;
	case PROP_DISCOVERY:
		g_value_set_boxed (value, realm_kerberos_get_discovery (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_kerberos_set_property (GObject *obj,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
	RealmKerberos *self = REALM_KERBEROS (obj);

	switch (prop_id) {
	case PROP_NAME:
		realm_dbus_realm_set_name (self->pv->realm_iface,
		                           g_value_get_string (value));
		break;
	case PROP_DISCOVERY:
		realm_kerberos_set_discovery (self, g_value_get_boxed (value));
		break;
	case PROP_PROVIDER:
		/* ignore */
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_kerberos_finalize (GObject *obj)
{
	RealmKerberos *self = REALM_KERBEROS (obj);

	g_object_unref (self->pv->realm_iface);
	g_object_unref (self->pv->kerberos_iface);
	if (self->pv->membership_iface)
		g_object_unref (self->pv->membership_iface);

	if (self->pv->discovery)
		g_hash_table_unref (self->pv->discovery);

	G_OBJECT_CLASS (realm_kerberos_parent_class)->finalize (obj);
}

static void
realm_kerberos_class_init (RealmKerberosClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GDBusObjectSkeletonClass *skeleton_class = G_DBUS_OBJECT_SKELETON_CLASS (klass);

	object_class->constructed = realm_kerberos_constructed;
	object_class->get_property = realm_kerberos_get_property;
	object_class->set_property = realm_kerberos_set_property;
	object_class->finalize = realm_kerberos_finalize;

	skeleton_class->authorize_method = realm_kerberos_authorize_method;

	g_type_class_add_private (klass, sizeof (RealmKerberosPrivate));

	g_object_class_install_property (object_class, PROP_NAME,
	             g_param_spec_string ("name", "Name", "Name",
	                                  NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_DISCOVERY,
	             g_param_spec_boxed ("discovery", "Discovery", "Discovery Data",
	                                 G_TYPE_HASH_TABLE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (object_class, PROP_PROVIDER,
	            g_param_spec_object ("provider", "Provider", "Realm Provider",
	                                 REALM_TYPE_PROVIDER, G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

void
realm_kerberos_set_discovery (RealmKerberos *self,
                              GHashTable *discovery)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));

	if (discovery)
		g_hash_table_ref (discovery);
	if (self->pv->discovery)
		g_hash_table_unref (self->pv->discovery);
	self->pv->discovery = discovery;
	g_object_notify (G_OBJECT (self), "discovery");
}

GHashTable *
realm_kerberos_get_discovery (RealmKerberos *self)
{
	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);
	return self->pv->discovery;
}

gchar **
realm_kerberos_parse_logins (RealmKerberos *self,
                             gboolean lower,
                             const gchar **logins,
                             GError **error)
{
	const gchar *failed = NULL;
	const gchar *const *formats;
	gchar **result;

	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);

	formats = realm_dbus_realm_get_login_formats (self->pv->realm_iface);
	if (formats == NULL) {
		g_set_error (error, REALM_ERROR,
		             REALM_ERROR_NOT_CONFIGURED,
		             _("The realm does not allow specifying logins"));
		return NULL;
	}

	result = realm_login_name_parse_all (formats, lower, logins, &failed);
	if (result == NULL) {
		g_set_error (error, G_DBUS_ERROR,
		             G_DBUS_ERROR_INVALID_ARGS,
		             _("Invalid login argument%s%s%s does not match the login format."),
		             failed ? " '" : "", failed, failed ? "'" : "");
	}

	return result;
}

gchar *
realm_kerberos_format_login (RealmKerberos *self,
                             const gchar *user)
{
	const gchar *const *formats;

	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);
	g_return_val_if_fail (user != NULL, NULL);

	formats = realm_dbus_realm_get_login_formats (self->pv->realm_iface);
	if (formats == NULL || formats[0] == NULL)
		return NULL;

	return realm_login_name_format (formats[0], user);
}

typedef struct {
	GDBusMethodInvocation *invocation;
	gchar *principal;
	GBytes *password;
	krb5_enctype *enctypes;
	gint n_enctypes;
	gchar *ccache_file;
} KinitClosure;

static void
kinit_closure_free (gpointer data)
{
	KinitClosure *kinit = data;
	g_object_unref (kinit->invocation);
	g_free (kinit->principal);
	g_bytes_unref (kinit->password);
	g_free (kinit->enctypes);
	realm_credential_ccache_delete_and_free (kinit->ccache_file);
	g_slice_free (KinitClosure, kinit);
}

static void
set_krb5_error (GError **error,
                krb5_error_code code,
                krb5_context context,
                const gchar *message,
                ...) G_GNUC_PRINTF (4, 5);

static void
set_krb5_error (GError **error,
                krb5_error_code code,
                krb5_context context,
                const gchar *message,
                ...)
{
	gchar *string;
	va_list va;

	va_start (va, message);
	string = g_strdup_vprintf (message, va);
	va_end (va);

	g_set_error (error, REALM_KRB5_ERROR, code,
	             "%s: %s", string, krb5_get_error_message (context, code));
	g_free (string);
}

static krb5_error_code
bytes_prompter(krb5_context context,
               void *data,
               const char *name,
               const char *banner,
               int num_prompts,
               krb5_prompt prompts[])
{
	krb5_prompt_type *prompt_types;
	const guchar *password;
	gsize length;
	gint i;

	/* Pull out the password data */
	password = g_bytes_get_data (data, &length);

	prompt_types = krb5_get_prompt_types (context);
	g_return_val_if_fail (prompt_types != NULL, KRB5_LIBOS_CANTREADPWD);

	for (i = 0; i < num_prompts; i++) {
		if (prompt_types[i] == KRB5_PROMPT_TYPE_PASSWORD) {
			if (length > prompts[i].reply->length) {
				g_warning ("Password too long for kerberos library");
				return KRB5_LIBOS_CANTREADPWD;
			}
			memcpy (prompts[i].reply->data, password, length);
			prompts[i].reply->length = length;
		} else {
			prompts[i].reply->length = 0;
		}
	}

	return 0;
}

static void
kinit_ccache_thread_func (GSimpleAsyncResult *async,
                          GObject *object,
                          GCancellable *cancellable)
{
	KinitClosure *kinit = g_simple_async_result_get_op_res_gpointer (async);
	krb5_get_init_creds_opt *options = NULL;
	krb5_context context = NULL;
	krb5_principal principal = NULL;
	krb5_error_code code;
	krb5_ccache ccache = NULL;
	krb5_creds my_creds;
	GError *error = NULL;
	int temp_fd;

	code = krb5_init_context (&context);
	if (code != 0) {
		set_krb5_error (&error, code, NULL, "Couldn't initialize kerberos");
		g_simple_async_result_take_error (async, error);
		goto cleanup;
	}

	code = krb5_parse_name (context, kinit->principal, &principal);
	if (code != 0) {
		set_krb5_error (&error, code, context, "Couldn't parse principal: %s", kinit->principal);
		g_simple_async_result_take_error (async, error);
		goto cleanup;
	}

	code = krb5_get_init_creds_opt_alloc (context, &options);
	warn_if_krb5_failed (context, code);

	kinit->ccache_file = g_build_filename (g_get_tmp_dir (), "realmd-krb5-cache.XXXXXX", NULL);
	temp_fd = g_mkstemp_full (kinit->ccache_file, O_RDWR, S_IRUSR | S_IWUSR);
	if (temp_fd == -1) {
		g_simple_async_result_set_error (async, G_FILE_ERROR, g_file_error_from_errno (errno),
		                                 "Couldn't create credential cache file: %s",
		                                 g_strerror (errno));
		goto cleanup;
	}
	close (temp_fd);

	code = krb5_cc_resolve (context, kinit->ccache_file, &ccache);
	if (code != 0) {
		set_krb5_error (&error, code, context, "Couldn't resolve credential cache: %s", kinit->ccache_file);
		g_simple_async_result_take_error (async, error);
		goto cleanup;
	}

	if (kinit->enctypes)
		krb5_get_init_creds_opt_set_etype_list (options, kinit->enctypes, kinit->n_enctypes);

	code = krb5_get_init_creds_opt_set_out_ccache (context, options, ccache);
	warn_if_krb5_failed (context, code);

	code = krb5_get_init_creds_password (context, &my_creds, principal,
	                                     NULL, bytes_prompter, kinit->password,
	                                     0, NULL, options);
	if (code != 0) {
		set_krb5_error (&error, code, context, "Couldn't authenticate as: %s", kinit->principal);
		g_simple_async_result_take_error (async, error);
		goto cleanup;
	}

	krb5_cc_close (context, ccache);
	ccache = NULL;

cleanup:
	if (options)
		krb5_get_init_creds_opt_free (context, options);
	if (principal)
		krb5_free_principal (context, principal);
	if (ccache)
		krb5_cc_close (context, ccache);
	if (context)
		krb5_free_context (context);
}

void
realm_kerberos_kinit_ccache_async (RealmKerberos *self,
                                   const gchar *name,
                                   GBytes *password,
                                   const krb5_enctype *enctypes,
                                   GDBusMethodInvocation *invocation,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data)
{
	GSimpleAsyncResult *async;
	KinitClosure *kinit;

	g_return_if_fail (REALM_IS_KERBEROS (self));
	g_return_if_fail (name != NULL);
	g_return_if_fail (password != NULL);

	async = g_simple_async_result_new (G_OBJECT (self), callback, user_data,
	                                   realm_kerberos_kinit_ccache_async);
	kinit = g_slice_new0 (KinitClosure);
	kinit->password = g_bytes_ref (password);
	kinit->invocation = g_object_ref (invocation);

	if (enctypes != NULL) {
		while (enctypes[kinit->n_enctypes])
			kinit->n_enctypes++;
		kinit->enctypes = g_memdup (enctypes, sizeof (krb5_enctype) * kinit->n_enctypes);
	}

	if (strchr (name, '@') == NULL) {
		kinit->principal = g_strdup_printf ("%s@%s", name,
		                                    realm_kerberos_get_realm_name (self));
	} else {
		kinit->principal = g_strdup (name);
	}

	g_simple_async_result_set_op_res_gpointer (async, kinit, kinit_closure_free);
	g_simple_async_result_run_in_thread (async, kinit_ccache_thread_func, G_PRIORITY_DEFAULT, NULL);
	g_object_unref (async);
}

gchar *
realm_kerberos_kinit_ccache_finish (RealmKerberos *self,
                                    GAsyncResult *result,
                                    GError **error)
{
	GSimpleAsyncResult *async;
	KinitClosure *kinit;
	GError *krb5_error = NULL;
	gchar *filename;

	g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
	                      realm_kerberos_kinit_ccache_async), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	async = G_SIMPLE_ASYNC_RESULT (result);
	kinit = g_simple_async_result_get_op_res_gpointer (async);

	if (g_simple_async_result_propagate_error (async, &krb5_error)) {
		realm_diagnostics_error (kinit->invocation, krb5_error, NULL);

		if (g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_PREAUTH_FAILED) ||
		    g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_C_PRINCIPAL_UNKNOWN) ||
		    g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_KEY_EXP) ||
		    g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_CLIENT_REVOKED) ||
		    g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_POLICY) ||
		    g_error_matches (krb5_error, REALM_KRB5_ERROR, KRB5KDC_ERR_ETYPE_NOSUPP)) {
			g_set_error (error, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
			             "Couldn't authenticate as: %s: %s", kinit->principal,
			             krb5_error->message);
			g_error_free (krb5_error);
			return NULL;
		}

		g_propagate_error (error, krb5_error);
		return NULL;
	}

	filename = kinit->ccache_file;
	kinit->ccache_file = NULL;
	return filename;
}

const gchar *
realm_kerberos_get_name (RealmKerberos *self)
{
	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);
	return realm_dbus_realm_get_name (self->pv->realm_iface);
}

const gchar *
realm_kerberos_get_realm_name (RealmKerberos *self)
{
	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);
	return realm_dbus_kerberos_get_realm_name (self->pv->kerberos_iface);
}

void
realm_kerberos_set_realm_name (RealmKerberos *self,
                               const gchar *value)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	realm_dbus_kerberos_set_realm_name (self->pv->kerberos_iface, value);
}

void
realm_kerberos_set_domain_name (RealmKerberos *self,
                                const gchar *value)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	realm_dbus_kerberos_set_domain_name (self->pv->kerberos_iface, value);
}

void
realm_kerberos_set_suggested_admin (RealmKerberos *self,
                                    const gchar *value)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	g_return_if_fail (self->pv->membership_iface != NULL);
	realm_dbus_kerberos_membership_set_suggested_administrator (self->pv->membership_iface, value);
}

void
realm_kerberos_set_permitted_logins (RealmKerberos *self,
                                     const gchar **value)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	realm_dbus_realm_set_permitted_logins (self->pv->realm_iface, (const gchar * const*)value);
}

const gchar *
realm_kerberos_login_policy_to_string (RealmKerberosLoginPolicy value)
{
	switch (value) {
	case REALM_KERBEROS_ALLOW_ANY_LOGIN:
		return REALM_DBUS_LOGIN_POLICY_ANY;
	case REALM_KERBEROS_ALLOW_REALM_LOGINS:
		return REALM_DBUS_LOGIN_POLICY_REALM;
	case REALM_KERBEROS_ALLOW_PERMITTED_LOGINS:
		return REALM_DBUS_LOGIN_POLICY_PERMITTED;
	case REALM_KERBEROS_DENY_ANY_LOGIN:
		return REALM_DBUS_LOGIN_POLICY_DENY;
	case REALM_KERBEROS_POLICY_NOT_SET:
		return "";
	default:
		g_return_val_if_reached ("");
	}
}

void
realm_kerberos_set_login_policy (RealmKerberos *self,
                                 RealmKerberosLoginPolicy value)
{
	realm_dbus_realm_set_login_policy (self->pv->realm_iface,
	                                   realm_kerberos_login_policy_to_string (value));
}

void
realm_kerberos_set_login_formats (RealmKerberos *self,
                                  const gchar **value)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	realm_dbus_realm_set_login_formats (self->pv->realm_iface, (const gchar * const*)value);
}

void
realm_kerberos_set_details (RealmKerberos *self,
                            ...)
{
	GPtrArray *tuples;
	GVariant *tuple;
	GVariant *details;
	const gchar *name;
	const gchar *value;
	GVariant *values[2];
	va_list va;

	g_return_if_fail (REALM_IS_KERBEROS (self));

	va_start (va, self);
	tuples = g_ptr_array_new ();

	for (;;) {
		name = va_arg (va, const gchar *);
		if (name == NULL)
			break;
		value = va_arg (va, const gchar *);
		g_return_if_fail (value != NULL);

		values[0] = g_variant_new_string (name);
		values[1] = g_variant_new_string (value);
		tuple = g_variant_new_tuple (values, 2);
		g_ptr_array_add (tuples, tuple);
	}
	va_end (va);

	details = g_variant_new_array (G_VARIANT_TYPE ("(ss)"),
	                               (GVariant * const *)tuples->pdata,
	                               tuples->len);

	realm_dbus_realm_set_details (self->pv->realm_iface, details);

	g_ptr_array_free (tuples, TRUE);
}

gboolean
realm_kerberos_is_configured (RealmKerberos *self)
{
	const gchar *configured;

	g_return_val_if_fail (REALM_IS_KERBEROS (self), FALSE);
	configured = realm_dbus_realm_get_configured (self->pv->realm_iface);
	return configured && !g_str_equal (configured, "");
}

void
realm_kerberos_set_configured (RealmKerberos *self,
                               gboolean configured)
{
	g_return_if_fail (REALM_IS_KERBEROS (self));
	realm_dbus_realm_set_configured (self->pv->realm_iface,
	                                 configured ? REALM_DBUS_KERBEROS_MEMBERSHIP_INTERFACE : "");
}

void
realm_kerberos_set_required_package_sets (RealmKerberos *self,
                                          const gchar **package_sets)
{
	gchar **packages;

	g_return_if_fail (REALM_IS_KERBEROS (self));
	packages = realm_packages_expand_sets (package_sets);
	realm_dbus_realm_set_required_packages (self->pv->realm_iface, (const gchar **)packages);
	g_strfreev (packages);
}

gchar *
realm_kerberos_calculate_join_computer_ou (RealmKerberos *self,
                                           GVariant *options)
{
	const gchar *computer_ou = NULL;

	g_return_val_if_fail (REALM_IS_KERBEROS (self), NULL);

	if (options) {
		if (!g_variant_lookup (options, REALM_DBUS_OPTION_COMPUTER_OU, "&s", &computer_ou))
			computer_ou = NULL;
	}

	if (!computer_ou)
		computer_ou = realm_settings_value (realm_kerberos_get_name (self), REALM_DBUS_OPTION_COMPUTER_OU);

	return g_strdup (computer_ou);
}

static gboolean
flush_keytab_entries (krb5_context ctx,
                      krb5_keytab keytab,
                      krb5_principal realm_princ,
                      GError **error)
{
	krb5_error_code code;
	krb5_kt_cursor cursor;
	krb5_keytab_entry entry;

	code = krb5_kt_start_seq_get (ctx, keytab, &cursor);
	if (code == KRB5_KT_END || code == ENOENT )
		return TRUE;

	while (!krb5_kt_next_entry (ctx, keytab, &entry, &cursor)) {
		if (krb5_realm_compare (ctx, realm_princ, entry.principal)) {
			code = krb5_kt_end_seq_get (ctx, keytab, &cursor);
			return_val_if_krb5_failed (ctx, code, FALSE);

			code = krb5_kt_remove_entry (ctx, keytab, &entry);
			return_val_if_krb5_failed (ctx, code, FALSE);

			code = krb5_kt_start_seq_get (ctx, keytab, &cursor);
			return_val_if_krb5_failed (ctx, code, FALSE);
		}

		code = krb5_kt_free_entry (ctx, &entry);
		return_val_if_krb5_failed (ctx, code, FALSE);
	}

	code = krb5_kt_end_seq_get (ctx, keytab, &cursor);
	return_val_if_krb5_failed (ctx, code, FALSE);

	return TRUE;
}

gboolean
realm_kerberos_flush_keytab (const gchar *realm_name,
                             GError **error)
{
	krb5_error_code code;
	krb5_keytab keytab;
	krb5_context ctx;
	krb5_principal princ;
	gchar *name;
	gboolean ret;

	code = krb5_init_context (&ctx);
	if (code != 0) {
		set_krb5_error (error, code, NULL, "Couldn't initialize kerberos");
		return FALSE;
	}

	code = krb5_kt_default (ctx, &keytab);
	if (code != 0) {
		set_krb5_error (error, code, NULL, "Couldn't open default host keytab");
		krb5_free_context (ctx);
		return FALSE;
	}

	name = g_strdup_printf ("user@%s", realm_name);
	code = krb5_parse_name (ctx, name, &princ);
	return_val_if_krb5_failed (ctx, code, FALSE);
	g_free (name);

	ret = flush_keytab_entries (ctx, keytab, princ, error);
	krb5_free_principal (ctx, princ);

	code = krb5_kt_close (ctx, keytab);
	warn_if_krb5_failed (ctx, code);

	krb5_free_context (ctx);
	return ret;

}
