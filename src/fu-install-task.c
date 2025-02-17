/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuInstallTask"

#include "config.h"

#include <fwupd.h>

#include "fu-common.h"
#include "fu-common-version.h"
#include "fu-device-private.h"
#include "fu-install-task.h"
#include "fu-keyring-utils.h"

struct _FuInstallTask
{
	GObject			 parent_instance;
	FuDevice		*device;
	XbNode			*component;
	FwupdReleaseFlags		 trust_flags;
	gboolean		 is_downgrade;
};

G_DEFINE_TYPE (FuInstallTask, fu_install_task, G_TYPE_OBJECT)

/**
 * fu_install_task_get_device:
 * @self: a #FuInstallTask
 *
 * Gets the device for this task.
 *
 * Returns: (transfer none): the device
 **/
FuDevice *
fu_install_task_get_device (FuInstallTask *self)
{
	g_return_val_if_fail (FU_IS_INSTALL_TASK (self), NULL);
	return self->device;
}

/**
 * fu_install_task_get_component:
 * @self: a #FuInstallTask
 *
 * Gets the component for this task.
 *
 * Returns: (transfer none): the component
 **/
XbNode *
fu_install_task_get_component (FuInstallTask *self)
{
	g_return_val_if_fail (FU_IS_INSTALL_TASK (self), NULL);
	return self->component;
}

/**
 * fu_install_task_get_trust_flags:
 * @self: a #FuInstallTask
 *
 * Gets the trust flags for this task.
 *
 * NOTE: This is only set after fu_install_task_check_requirements() has been
 * called successfully.
 *
 * Returns: the #FwupdReleaseFlags, e.g. #FWUPD_TRUST_FLAG_PAYLOAD
 **/
FwupdReleaseFlags
fu_install_task_get_trust_flags (FuInstallTask *self)
{
	g_return_val_if_fail (FU_IS_INSTALL_TASK (self), FALSE);
	return self->trust_flags;
}

/**
 * fu_install_task_get_is_downgrade:
 * @self: a #FuInstallTask
 *
 * Gets if this task is to downgrade firmware.
 *
 * NOTE: This is only set after fu_install_task_check_requirements() has been
 * called successfully.
 *
 * Returns: %TRUE if versions numbers are going backwards
 **/
gboolean
fu_install_task_get_is_downgrade (FuInstallTask *self)
{
	g_return_val_if_fail (FU_IS_INSTALL_TASK (self), FALSE);
	return self->is_downgrade;
}

static gchar *
fu_install_task_verfmts_to_string (GPtrArray *verfmts)
{
	GString *str = g_string_new (NULL);
	for (guint i = 0; i < verfmts->len; i++) {
		XbNode *verfmt = g_ptr_array_index (verfmts, i);
		const gchar *tmp = xb_node_get_text (verfmt);
		g_string_append_printf (str, "%s;", tmp);
	}
	if (str->len > 0)
		g_string_truncate (str, str->len - 1);
	return g_string_free (str, FALSE);
}

static gboolean
fu_install_task_check_verfmt (FuInstallTask *self,
			      GPtrArray *verfmts,
			      FwupdInstallFlags flags,
			      GError **error)
{
	FwupdVersionFormat fmt_dev = fu_device_get_version_format (self->device);
	g_autofree gchar *verfmts_str = NULL;

	/* no device format */
	if (fmt_dev == FWUPD_VERSION_FORMAT_UNKNOWN &&
	    (flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		verfmts_str = fu_install_task_verfmts_to_string (verfmts);
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "release version format '%s' but no device version format",
			     verfmts_str);
		return FALSE;
	}

	/* compare all version formats */
	for (guint i = 0; i < verfmts->len; i++) {
		XbNode *verfmt = g_ptr_array_index (verfmts, i);
		const gchar *tmp = xb_node_get_text (verfmt);
		FwupdVersionFormat fmt_rel = fwupd_version_format_from_string (tmp);
		if (fmt_dev == fmt_rel)
			return TRUE;
	}
	verfmts_str = fu_install_task_verfmts_to_string (verfmts);
	if ((flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Firmware version formats were different, "
			     "device was '%s' and release is '%s'",
			     fwupd_version_format_to_string (fmt_dev),
			     verfmts_str);
		return FALSE;
	}
	g_warning ("ignoring version format difference %s:%s",
		   fwupd_version_format_to_string (fmt_dev),
		   verfmts_str);
	return TRUE;
}

static gboolean
fu_install_task_check_requirements_version_check (FuInstallTask *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) reqs = NULL;

	reqs = xb_node_query (fu_install_task_get_component (self),
			      "requires/*", 0, &error_local);
	if (reqs == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     error_local->message);
		return FALSE;
	}
	for (guint i = 0; i < reqs->len; i++) {
		XbNode *req = g_ptr_array_index (reqs, i);
		if (g_strcmp0 (xb_node_get_element (req), "firmware") == 0 &&
		    xb_node_get_text (req) == NULL) {
			return TRUE;
		}
	}
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "no firmware requirement");
	return FALSE;
}

/**
 * fu_install_task_check_requirements:
 * @self: a #FuInstallTask
 * @flags: install flags, e.g. #FWUPD_INSTALL_FLAG_ALLOW_OLDER
 * @error: (nullable): optional return location for an error
 *
 * Checks any requirements of this task. This will typically involve checking
 * that the device can accept the component (the GUIDs match) and that the
 * device can be upgraded with this firmware version.
 *
 * Returns: %TRUE if the requirements passed
 **/
gboolean
fu_install_task_check_requirements (FuInstallTask *self,
				    FwupdInstallFlags flags,
				    GError **error)
{
	const gchar *branch_new;
	const gchar *branch_old;
	const gchar *protocol;
	const gchar *version;
	const gchar *version_release_raw;
	const gchar *version_lowest;
	gboolean matches_guid = FALSE;
	gint vercmp;
	g_autofree gchar *version_release = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) provides = NULL;
	g_autoptr(GPtrArray) verfmts = NULL;
	g_autoptr(XbNode) release = NULL;
#if LIBXMLB_CHECK_VERSION(0,2,0)
	g_autoptr(XbQuery) query = NULL;
#endif

	g_return_val_if_fail (FU_IS_INSTALL_TASK (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* does this component provide a GUID the device has */
	provides = xb_node_query (self->component,
				  "provides/firmware[@type='flashed']",
				  0, &error_local);
	if (provides == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "No supported devices found: %s",
			     error_local->message);
		return FALSE;
	}
	for (guint i = 0; i < provides->len; i++) {
		XbNode *provide = g_ptr_array_index (provides, i);
		if (fu_device_has_guid (self->device, xb_node_get_text (provide))) {
			matches_guid = TRUE;
			break;
		}
	}
	if (!matches_guid) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_FOUND,
				     "No supported devices found");
		return FALSE;
	}

	/* device requires a version check */
	if (fu_device_has_flag (self->device, FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED)) {
		if (!fu_install_task_check_requirements_version_check (self, error)) {
			g_prefix_error (error, "device requires firmware with a version check: ");
			return FALSE;
		}
	}

	/* does the protocol match */
	protocol = xb_node_query_text (self->component,
				       "custom/value[@key='LVFS::UpdateProtocol']",
				       NULL);
	if (fu_device_get_protocols (self->device)->len != 0 && protocol != NULL &&
	    !fu_device_has_protocol (self->device, protocol) &&
	    (flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_autofree gchar *str = NULL;
		str = fu_common_strjoin_array ("|", fu_device_get_protocols (self->device));
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s does not support %s, only %s",
			     fu_device_get_name (self->device),
			     protocol, str);
		return FALSE;
	}

	/* check the device is not locked */
	if (fu_device_has_flag (self->device, FWUPD_DEVICE_FLAG_LOCKED)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s [%s] is locked",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device));
		return FALSE;
	}

	/* check the branch is not switching */
	branch_new = xb_node_query_text (self->component, "branch", NULL);
	branch_old = fu_device_get_branch (self->device);
	if ((flags & FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH) == 0 &&
	    g_strcmp0 (branch_old, branch_new) != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s [%s] would switch firmware branch from %s to %s",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device),
			     branch_old != NULL ? branch_old : "default",
			     branch_new != NULL ? branch_new : "default");
		return FALSE;
	}

	/* no update abilities */
	if (!fu_device_has_flag (self->device, FWUPD_DEVICE_FLAG_UPDATABLE)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s [%s] does not currently allow updates",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device));
		return FALSE;
	}

	/* called with online update, test if device is supposed to allow this */
	if ((flags & FWUPD_INSTALL_FLAG_OFFLINE) == 0 &&
	    (flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    fu_device_has_flag (self->device, FWUPD_DEVICE_FLAG_ONLY_OFFLINE)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s [%s] only allows offline updates",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device));
		return FALSE;
	}

	/* get device */
	version = fu_device_get_version (self->device);
	if (version == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Device %s [%s] has no firmware version",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device));
		return FALSE;
	}

	/* get latest release */
#if LIBXMLB_CHECK_VERSION(0,2,0)
	query = xb_query_new_full (xb_node_get_silo (self->component),
				   "releases/release",
				   XB_QUERY_FLAG_FORCE_NODE_CACHE,
				   error);
	if (query == NULL)
		return FALSE;
	release = xb_node_query_first_full (self->component, query, NULL);
#else
	release = xb_node_query_first (self->component, "releases/release", NULL);
#endif
	if (release == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "%s [%s] has no firmware update metadata",
			     fu_device_get_name (self->device),
			     fu_device_get_id (self->device));
		return FALSE;
	}

	/* is this a downgrade or re-install */
	version_release_raw = xb_node_get_attr (release, "version");
	if (version_release_raw == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "Release has no firmware version");
		return FALSE;
	}

	/* check the version formats match if set in the release */
	if ((flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    (flags & FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH) == 0) {
		verfmts = xb_node_query (self->component,
					"custom/value[@key='LVFS::VersionFormat']",
					0, NULL);
		if (verfmts != NULL) {
			if (!fu_install_task_check_verfmt (self, verfmts, flags, error))
				return FALSE;
		}
	}

	/* compare to the lowest supported version, if it exists */
	version_lowest = fu_device_get_version_lowest (self->device);
	if (version_lowest != NULL &&
	    fu_common_vercmp_full (version_lowest, version,
				   fu_device_get_version_format (self->device)) > 0 &&
	    (flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_VERSION_NEWER,
			     "Specified firmware is older than the minimum "
			     "required version '%s < %s'", version, version_lowest);
		return FALSE;
	}

	/* check semver */
	if (fu_device_get_version_format (self->device) == FWUPD_VERSION_FORMAT_PLAIN) {
		version_release = g_strdup (version_release_raw);
	} else {
		version_release = fu_common_version_parse_from_format (version_release_raw,
								       fu_device_get_version_format (self->device));
	}
	vercmp = fu_common_vercmp_full (version, version_release,
					fu_device_get_version_format (self->device));
	if (fu_device_has_flag(self->device, FWUPD_DEVICE_FLAG_ONLY_VERSION_UPGRADE) &&
	    vercmp >= 0) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "Device only supports version upgrades");
		return FALSE;
	}
	if (vercmp == 0 && (flags & FWUPD_INSTALL_FLAG_ALLOW_REINSTALL) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_VERSION_SAME,
			     "Specified firmware is already installed '%s'",
			     version_release);
		return FALSE;
	}
	self->is_downgrade = vercmp > 0;
	if (self->is_downgrade &&
	    (flags & FWUPD_INSTALL_FLAG_ALLOW_OLDER) == 0 &&
	    (flags & FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_VERSION_NEWER,
			     "Specified firmware is older than installed '%s < %s'",
			     version_release, version);
		return FALSE;
	}

	/* verify */
	if (!fu_keyring_get_release_flags (release, &self->trust_flags, &error_local)) {
		if (g_error_matches (error_local, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
			g_warning ("Ignoring verification for %s: %s",
				   fu_device_get_name (self->device),
				   error_local->message);
		} else {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * fu_install_task_get_action_id:
 * @self: a #FuEngine
 *
 * Gets the PolicyKit action ID to use for the install operation.
 *
 * Returns: string, e.g. `org.freedesktop.fwupd.update-internal-trusted`
 **/
const gchar *
fu_install_task_get_action_id (FuInstallTask *self)
{
	/* relax authentication checks for removable devices */
	if (!fu_device_has_flag (self->device, FWUPD_DEVICE_FLAG_INTERNAL)) {
		if (self->is_downgrade)
			return "org.freedesktop.fwupd.downgrade-hotplug";
		if (self->trust_flags & FWUPD_TRUST_FLAG_PAYLOAD)
			return "org.freedesktop.fwupd.update-hotplug-trusted";
		return "org.freedesktop.fwupd.update-hotplug";
	}

	/* internal device */
	if (self->is_downgrade)
		return "org.freedesktop.fwupd.downgrade-internal";
	if (self->trust_flags & FWUPD_TRUST_FLAG_PAYLOAD)
		return "org.freedesktop.fwupd.update-internal-trusted";
	return "org.freedesktop.fwupd.update-internal";
}

static void
fu_install_task_init (FuInstallTask *self)
{
	self->trust_flags = FWUPD_TRUST_FLAG_NONE;
}

static void
fu_install_task_finalize (GObject *object)
{
	FuInstallTask *self = FU_INSTALL_TASK (object);

	if (self->component != NULL)
		g_object_unref (self->component);
	if (self->device != NULL)
		g_object_unref (self->device);

	G_OBJECT_CLASS (fu_install_task_parent_class)->finalize (object);
}

static void
fu_install_task_class_init (FuInstallTaskClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = fu_install_task_finalize;
}

/**
 * fu_install_task_compare:
 * @task1: first task to compare.
 * @task2: second task to compare.
 *
 * Compares two install tasks.
 *
 * Returns: 1, 0 or -1 if @task1 is greater, equal, or less than @task2, respectively.
 **/
gint
fu_install_task_compare (FuInstallTask *task1, FuInstallTask *task2)
{
	FuDevice *device1 = fu_install_task_get_device (task1);
	FuDevice *device2 = fu_install_task_get_device (task2);
	if (fu_device_get_order (device1) < fu_device_get_order (device2))
		return -1;
	if (fu_device_get_order (device1) > fu_device_get_order (device2))
		return 1;
	return 0;
}

/**
 * fu_install_task_new:
 * @device: a device
 * @component: a Xmlb node
 *
 * Creates a new install task that may or may not be valid.
 *
 * Returns: (transfer full): the #FuInstallTask
 **/
FuInstallTask *
fu_install_task_new (FuDevice *device, XbNode *component)
{
	FuInstallTask *self;
	self = g_object_new (FU_TYPE_TASK, NULL);
	if (component != NULL)
		self->component = g_object_ref (component);
	if (device != NULL)
		self->device = g_object_ref (device);
	return FU_INSTALL_TASK (self);
}
