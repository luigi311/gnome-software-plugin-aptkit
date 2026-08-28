/*
 * Copyright (C) 2025 Bardia Moshiri <bardia@furilabs.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gs-plugin-aptkit.h"
#include <appstream.h>
#include <glib/gi18n.h>
#include <gnome-software.h>
#include <gs-app-list.h>
#include <gs-app-query.h>

typedef enum {
  ACTION_UPDATE_CACHE,
  ACTION_UPGRADE_SYSTEM,
  ACTION_LIST_UPDATES,
  ACTION_FIX_BROKEN_DEPENDS
} TransactionAction;

struct _GsPluginAptkit
{
  GsPlugin parent;

  GDBusProxy *aptkit_proxy;  /* Proxy for Aptkit */
  GsAppList *updatable_apps;  /* List of apps with updates */
  gboolean tried_safe_mode;  /* Flag to track if safe mode has been tried */
};

typedef struct {
  GTask *task;
  GsPluginAptkit *plugin;
  TransactionAction action;
  gboolean safe_mode;  /* Flag to indicate if safe mode is enabled */
} TransactionData;

G_DEFINE_TYPE (GsPluginAptkit, gs_plugin_aptkit, GS_TYPE_PLUGIN);

static void
aptkit_upgrade_system_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data);

static void
aptkit_proxy_setup_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (GTask) task = g_steal_pointer (&user_data);
  GsPluginAptkit *self = g_task_get_source_object (task);
  g_autoptr (GError) error = NULL;
  GDBusProxy *proxy;

  proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_clear_object (&self->aptkit_proxy);
  self->aptkit_proxy = proxy;

  g_task_return_boolean (task, TRUE);
}

static gboolean
gs_plugin_aptkit_setup_finish (GsPlugin *plugin,
                               GAsyncResult *result,
                               GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_aptkit_setup_async (GsPlugin *plugin,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_aptkit_setup_async);

  g_debug ("Aptkit plugin version: %s", GS_PLUGIN_APTKIT_VERSION);

  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                            G_DBUS_PROXY_FLAGS_NONE,
                            NULL,
                            "org.aptkit",
                            "/org/aptkit",
                            "org.aptkit",
                            cancellable,
                            aptkit_proxy_setup_cb,
                            g_steal_pointer (&task));
}

static gboolean
gs_plugin_aptkit_refresh_metadata_finish (GsPlugin *plugin,
                                          GAsyncResult *result,
                                          GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static gboolean
aptkit_process_packages (GsPluginAptkit *plugin,
                         GsAppList *list,
                         GVariant *packages,
                         GVariant *dependencies)
{
  g_autoptr(GVariant) pkg_upgrades = NULL;
  g_autoptr(GVariant) dep_upgrades = NULL;
  g_autoptr(GVariant) pkg_downgrades = NULL;
  g_autoptr(GVariant) dep_downgrades = NULL;
  GVariantIter iter;
  const gchar *package_name;
  gboolean added_any_packages = FALSE;

  gs_app_list_remove_all (plugin->updatable_apps);

  /* Get the upgrades arrays (fifth element) */
  pkg_upgrades = g_variant_get_child_value (packages, 4);
  dep_upgrades = g_variant_get_child_value (dependencies, 4);

  /* Get the downgrades arrays (sixth element, index 5) */
  pkg_downgrades = g_variant_get_child_value (packages, 5);
  dep_downgrades = g_variant_get_child_value (dependencies, 5);

  /* Process upgrades */
  g_variant_iter_init (&iter, pkg_upgrades);
  while (g_variant_iter_next (&iter, "&s", &package_name)) {
    g_autoptr(GsApp) app = NULL;

    app = gs_app_new (package_name);
    gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
    gs_app_set_allow_cancel (app, FALSE);
    gs_app_set_management_plugin (app, GS_PLUGIN (plugin));
    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, package_name);
    gs_app_set_metadata (app, "aptkit::package-name", package_name);
    gs_app_add_source (app, package_name);
    gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "deb");
    gs_app_set_state (app, GS_APP_STATE_UPDATABLE_LIVE);
    gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

    gs_app_list_add (list, app);
    gs_app_list_add (plugin->updatable_apps, app);
    added_any_packages = TRUE;
  }

  /* Process package downgrades */
  g_variant_iter_init (&iter, pkg_downgrades);
  while (g_variant_iter_next (&iter, "&s", &package_name)) {
    g_autoptr(GsApp) app = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *name;
    const gchar *version;

    /* Parse package name and version from format "name=version" */
    parts = g_strsplit (package_name, "=", 2);
    if (parts == NULL || parts[0] == NULL)
      continue;

    name = parts[0];
    version = parts[1];

    app = gs_app_new (name);
    gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
    gs_app_set_allow_cancel (app, FALSE);
    gs_app_set_management_plugin (app, GS_PLUGIN (plugin));
    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
    gs_app_set_metadata (app, "aptkit::package-name", name);
    gs_app_add_source (app, name);
    gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "deb");
    gs_app_set_state (app, GS_APP_STATE_UPDATABLE_LIVE);
    gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

    if (version != NULL)
      gs_app_set_update_version (app, version);

    gs_app_list_add (list, app);
    gs_app_list_add (plugin->updatable_apps, app);
    added_any_packages = TRUE;
  }

  /* Process dependency upgrades */
  g_variant_iter_init (&iter, dep_upgrades);
  while (g_variant_iter_next (&iter, "&s", &package_name)) {
    g_autoptr(GsApp) app = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *name;
    const gchar *version;

    parts = g_strsplit (package_name, "=", 2);
    if (parts == NULL || parts[0] == NULL)
      continue;

    name = parts[0];
    version = parts[1];

    app = gs_app_new (name);
    gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
    gs_app_set_allow_cancel (app, FALSE);
    gs_app_set_management_plugin (app, GS_PLUGIN (plugin));
    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
    gs_app_set_metadata (app, "aptkit::package-name", name);
    gs_app_add_source (app, name);
    gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "deb");
    gs_app_set_state (app, GS_APP_STATE_UPDATABLE_LIVE);
    gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

    if (version != NULL)
      gs_app_set_update_version (app, version);

    gs_app_list_add (list, app);
    gs_app_list_add (plugin->updatable_apps, app);
    added_any_packages = TRUE;
  }

  /* Process dependency downgrades */
  g_variant_iter_init (&iter, dep_downgrades);
  while (g_variant_iter_next (&iter, "&s", &package_name)) {
    g_autoptr(GsApp) app = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *name;
    const gchar *version;

    parts = g_strsplit (package_name, "=", 2);
    if (parts == NULL || parts[0] == NULL)
      continue;

    name = parts[0];
    version = parts[1];

    app = gs_app_new (name);
    gs_app_set_kind (app, AS_COMPONENT_KIND_GENERIC);
    gs_app_set_scope (app, AS_COMPONENT_SCOPE_SYSTEM);
    gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_PACKAGE);
    gs_app_set_allow_cancel (app, FALSE);
    gs_app_set_management_plugin (app, GS_PLUGIN (plugin));
    gs_app_set_name (app, GS_APP_QUALITY_NORMAL, name);
    gs_app_set_metadata (app, "aptkit::package-name", name);
    gs_app_add_source (app, name);
    gs_app_set_metadata (app, "GnomeSoftware::PackagingFormat", "deb");
    gs_app_set_state (app, GS_APP_STATE_UPDATABLE_LIVE);
    gs_app_add_kudo (app, GS_APP_KUDO_SANDBOXED_SECURE);

    if (version != NULL)
      gs_app_set_update_version (app, version);

    gs_app_list_add (list, app);
    gs_app_list_add (plugin->updatable_apps, app);
    added_any_packages = TRUE;
  }

  return added_any_packages;
}

static void
aptkit_transaction_signal_cb (GDBusProxy *proxy,
                              const gchar *sender_name,
                              const gchar *signal_name,
                              GVariant *parameters,
                              gpointer user_data)
{
  TransactionData *data = (TransactionData *)user_data;
  g_debug ("Received signal: %s", signal_name);

  if (g_strcmp0 (signal_name, "PropertyChanged") == 0) {
    const gchar *property_name;
    g_autoptr(GVariant) value = NULL;
    g_variant_get (parameters, "(&sv)", &property_name, &value);
    g_debug ("Property changed: %s", property_name);

    if (g_strcmp0 (property_name, "ExitState") == 0) {
      const gchar *exit_state;
      g_variant_get (value, "&s", &exit_state);
      g_debug ("Exit state changed to: %s", exit_state);

      if (g_strcmp0 (exit_state, "exit-success") == 0) {
        if (data->action == ACTION_FIX_BROKEN_DEPENDS) {
          GTask *task = data->task;
          GsPluginAptkit *plugin = data->plugin;
          GCancellable *cancellable = g_task_get_cancellable (task);
          TransactionAction original_action;

          original_action = GPOINTER_TO_INT (
              g_object_get_data (G_OBJECT (task), "aptkit-action-before-repair"));
          g_task_set_task_data (task, GINT_TO_POINTER (original_action), NULL);

          g_debug ("Broken dependencies repaired, retrying original action");

          g_signal_handlers_disconnect_by_data (proxy, data);
          g_object_unref (proxy);
          g_free (data);

          g_dbus_proxy_call (plugin->aptkit_proxy,
                             "UpgradeSystem",
                             g_variant_new ("(b)", GPOINTER_TO_INT (
                                 g_object_get_data (G_OBJECT (task), "safe-mode"))),
                             G_DBUS_CALL_FLAGS_NONE,
                             -1,
                             cancellable,
                             aptkit_upgrade_system_cb,
                             task);
          return;
        }

        /* we only need to emit updates changed on cache update or system upgrade */
        if (data->action == ACTION_UPGRADE_SYSTEM) {
          for (guint i = 0; i < gs_app_list_length (data->plugin->updatable_apps); i++) {
            GsApp *app = gs_app_list_index (data->plugin->updatable_apps, i);
            const gchar *package_name = gs_app_get_metadata_item (app, "aptkit::package-name");
            if (package_name != NULL) {
              g_debug ("Upgraded package: %s", package_name);
              gs_app_set_state (app, GS_APP_STATE_INSTALLED);
            }
          }

          gs_plugin_updates_changed (GS_PLUGIN (data->plugin));
        } else if (data->action == ACTION_UPDATE_CACHE) {
          gs_plugin_updates_changed (GS_PLUGIN (data->plugin));
        }

        g_task_return_boolean (data->task, TRUE);
      } else if (g_strcmp0 (exit_state, "exit-cancelled") == 0) {
        g_task_return_new_error (data->task,
                                 GS_PLUGIN_ERROR,
                                 GS_PLUGIN_ERROR_CANCELLED,
                                 "Transaction was cancelled");
      } else if (g_strcmp0 (exit_state, "exit-failed") == 0) {
        g_task_return_new_error (data->task,
                                 GS_PLUGIN_ERROR,
                                 GS_PLUGIN_ERROR_FAILED,
                                 "Transaction failed");
      } else if (g_strcmp0 (exit_state, "exit-previous-failed") == 0) {
        g_task_return_new_error (data->task,
                                 GS_PLUGIN_ERROR,
                                 GS_PLUGIN_ERROR_FAILED,
                                 "Previous transaction failed");
      }
      g_object_unref (proxy);
      g_free (data);
    } else if (g_strcmp0 (property_name, "Error") == 0) {
      const gchar *error_code;
      const gchar *error_details;

      g_variant_get (value, "(&s&s)", &error_code, &error_details);

      if (g_strcmp0 (error_code, "error-cache-broken") == 0 &&
          (data->action == ACTION_UPGRADE_SYSTEM ||
           data->action == ACTION_LIST_UPDATES) &&
          !GPOINTER_TO_INT (g_object_get_data (
              G_OBJECT (data->task), "aptkit-fix-broken-attempted"))) {
        GTask *task = data->task;
        GsPluginAptkit *plugin = data->plugin;
        GCancellable *cancellable = g_task_get_cancellable (task);

        g_debug ("Broken package dependencies detected: %s", error_details);
        g_object_set_data (G_OBJECT (task),
                           "aptkit-fix-broken-attempted",
                           GINT_TO_POINTER (TRUE));
        g_object_set_data (G_OBJECT (task),
                           "aptkit-action-before-repair",
                           GINT_TO_POINTER (data->action));
        g_task_set_task_data (task,
                              GINT_TO_POINTER (ACTION_FIX_BROKEN_DEPENDS),
                              NULL);

        g_signal_handlers_disconnect_by_data (proxy, data);
        g_object_unref (proxy);
        g_free (data);

        g_dbus_proxy_call (plugin->aptkit_proxy,
                           "FixBrokenDepends",
                           g_variant_new ("()"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           cancellable,
                           aptkit_upgrade_system_cb,
                           task);
      }
    } else if (g_strcmp0 (property_name, "Packages") == 0 ||
               g_strcmp0 (property_name, "Dependencies") == 0) {
      g_autoptr(GVariant) packages = NULL;
      g_autoptr(GVariant) dependencies = NULL;
      g_autoptr(GsAppList) list = gs_app_list_new ();

      /* Get both properties - one will be the 'value' parameter, get the other from proxy */
      if (g_strcmp0 (property_name, "Packages") == 0) {
        packages = g_variant_ref (value);
        dependencies = g_dbus_proxy_get_cached_property (proxy, "Dependencies");
      } else {
        dependencies = g_variant_ref (value);
        packages = g_dbus_proxy_get_cached_property (proxy, "Packages");
      }

      if (packages != NULL && dependencies != NULL) {
        gboolean has_packages = aptkit_process_packages (data->plugin, list, packages, dependencies);
        if (data->action == ACTION_LIST_UPDATES) {
          if (!has_packages && data->safe_mode && !data->plugin->tried_safe_mode) {
            /* If no packages found in safe mode, try without safe mode */
            data->plugin->tried_safe_mode = TRUE;
            g_debug ("No updates found in safe mode, trying without safe mode");

            /* Store needed references before freeing data */
            GTask *original_task = data->task;
            GDBusProxy *aptkit_proxy = data->plugin->aptkit_proxy;
            GCancellable *cancellable = g_task_get_cancellable (original_task);

            /* We need to clean up the current transaction before starting a new one */
            g_object_unref (proxy);
            g_free (data);

            /* Try listing updates without safe mode */
            g_dbus_proxy_call (aptkit_proxy,
                               "UpgradeSystem",
                               g_variant_new ("(b)", FALSE), /* safe mode off */
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               cancellable,
                               aptkit_upgrade_system_cb,
                               original_task);
          } else {
            /* Whether we found packages or not, return the list (which might be empty) */
            g_task_return_pointer (data->task, g_steal_pointer (&list), g_object_unref);
            g_object_unref (proxy);
            g_free (data);
          }
        }
      }
    }
  }
}

static void
aptkit_transaction_run_cb (GObject *source_object,
                           GAsyncResult *res,
                           gpointer user_data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (result == NULL)
    g_warning ("Failed to run transaction: %s", error->message);
}

static void
aptkit_transaction_proxy_updates_cb (GObject *source_object,
                                     GAsyncResult *res,
                                     gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  g_autoptr (GError) error = NULL;
  GDBusProxy *transaction_proxy;
  TransactionData *data;

  transaction_proxy = g_dbus_proxy_new_finish (res, &error);
  if (transaction_proxy == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  data = g_new0 (TransactionData, 1);
  data->task = task;
  data->plugin = GS_PLUGIN_APTKIT (g_task_get_source_object (task));
  data->action = GPOINTER_TO_INT (g_task_get_task_data (task));
  data->safe_mode = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "safe-mode"));

  g_signal_connect (transaction_proxy, "g-signal",
                    G_CALLBACK (aptkit_transaction_signal_cb),
                    data);

  const gchar *method = (data->action == ACTION_LIST_UPDATES) ? "Simulate" : "Run";
  g_debug ("Calling %s on transaction for action %d with safe mode %d",
           method, data->action, data->safe_mode);

  g_dbus_proxy_call (transaction_proxy,
                     method,
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     g_task_get_cancellable (task),
                     aptkit_transaction_run_cb,
                     NULL);
}

static void
aptkit_update_cache_cb (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (g_task_get_source_object (task));
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  const gchar *transaction_path;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (result == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_variant_get (result, "(&s)", &transaction_path);
  g_debug ("Got transaction path: %s", transaction_path);

  g_task_set_task_data (task, GINT_TO_POINTER (ACTION_UPDATE_CACHE), NULL);
  g_dbus_proxy_new (g_dbus_proxy_get_connection (self->aptkit_proxy),
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL,
                    "org.aptkit",
                    transaction_path,
                    "org.aptkit.transaction",
                    g_task_get_cancellable (task),
                    aptkit_transaction_proxy_updates_cb,
                    task);
}

static void
gs_plugin_aptkit_refresh_metadata_async (GsPlugin *plugin,
                                         guint64 cache_age_secs,
                                         GsPluginRefreshMetadataFlags flags,
                                         GsPluginEventCallback event_callback,
                                         void *event_user_data,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data)
{
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (plugin);
  g_autoptr (GTask) task = NULL;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_aptkit_refresh_metadata_async);

  g_debug ("Refreshing repositories");

  g_dbus_proxy_call (self->aptkit_proxy,
                     "UpdateCache",
                     g_variant_new ("()"),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     aptkit_update_cache_cb,
                     g_steal_pointer (&task));
}

static void
aptkit_upgrade_system_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (g_task_get_source_object (task));
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) result = NULL;
  const gchar *transaction_path;

  result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object), res, &error);
  if (result == NULL) {
    g_task_return_error (task, g_steal_pointer (&error));
    return;
  }

  g_variant_get (result, "(&s)", &transaction_path);
  g_debug ("Got transaction path: %s", transaction_path);

  g_dbus_proxy_new (g_dbus_proxy_get_connection (self->aptkit_proxy),
                    G_DBUS_PROXY_FLAGS_NONE,
                    NULL,
                    "org.aptkit",
                    transaction_path,
                    "org.aptkit.transaction",
                    g_task_get_cancellable (task),
                    aptkit_transaction_proxy_updates_cb,
                    task);
}

static GsAppList *
gs_plugin_aptkit_list_apps_finish (GsPlugin *plugin,
                                   GAsyncResult *result,
                                   GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gs_plugin_aptkit_list_apps_async (GsPlugin *plugin,
                                  GsAppQuery *query,
                                  GsPluginListAppsFlags flags,
                                  GsPluginEventCallback event_callback,
                                  void *event_user_data,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data)
{
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (plugin);
  g_autoptr (GTask) task = NULL;
  GsAppQueryTristate is_for_updates = GS_APP_QUERY_TRISTATE_UNSET;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_aptkit_list_apps_async);

  /* Reset the tried_safe_mode flag when listing apps */
  self->tried_safe_mode = FALSE;

  if (query != NULL)
    is_for_updates = gs_app_query_get_is_for_update (query);

  /* Currently only support one query type at a time */
  if (gs_app_query_get_n_properties_set (query) != 1 ||
      is_for_updates == GS_APP_QUERY_TRISTATE_FALSE) {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Unsupported query");
    return;
  }

  if (is_for_updates == GS_APP_QUERY_TRISTATE_TRUE) {
    g_debug ("Listing updates in safe mode first");

    /* Set safe mode flag */
    g_object_set_data (G_OBJECT (task), "safe-mode", GINT_TO_POINTER (TRUE));

    g_task_set_task_data (task, GINT_TO_POINTER (ACTION_LIST_UPDATES), NULL);
    g_dbus_proxy_call (self->aptkit_proxy,
                       "UpgradeSystem",
                       g_variant_new ("(b)", TRUE), /* safe mode */
                       G_DBUS_CALL_FLAGS_NONE,
                       -1,
                       cancellable,
                       aptkit_upgrade_system_cb,
                       g_steal_pointer (&task));
  } else {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Unsupported query type");
  }
}

static gboolean
gs_plugin_aptkit_filter_desktop_file_cb (GsPlugin *plugin,
                                         GsApp *app,
                                         const gchar *filename,
                                         GKeyFile *key_file,
                                         gpointer user_data)
{
  return strstr (filename, "/snapd/") == NULL &&
         strstr (filename, "/snap/") == NULL &&
         strstr (filename, "/flatpak/") == NULL &&
         g_key_file_has_group (key_file, "Desktop Entry") &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-Flatpak", NULL) &&
         !g_key_file_has_key (key_file, "Desktop Entry", "X-SnapInstanceName", NULL);
}

static gboolean
gs_plugin_aptkit_launch_finish (GsPlugin *plugin,
                                GAsyncResult *result,
                                GError **error)
{
  return gs_plugin_app_launch_filtered_finish (plugin, result, error);
}

static void
gs_plugin_aptkit_launch_async (GsPlugin *plugin,
                               GsApp *app,
                               GsPluginLaunchFlags flags,
                               GCancellable *cancellable,
                               GAsyncReadyCallback callback,
                               gpointer user_data)

{
  gs_plugin_app_launch_filtered_async (plugin, app, flags, gs_plugin_aptkit_filter_desktop_file_cb, NULL, cancellable, callback, user_data);
}

static gboolean
gs_plugin_aptkit_update_apps_finish (GsPlugin *plugin,
                                     GAsyncResult *result,
                                     GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gs_plugin_aptkit_update_apps_async (GsPlugin *plugin,
                                    GsAppList *list,
                                    GsPluginUpdateAppsFlags flags,
                                    GsPluginProgressCallback progress_callback,
                                    gpointer progress_user_data,
                                    GsPluginEventCallback event_callback,
                                    void *event_user_data,
                                    GsPluginAppNeedsUserActionCallback app_needs_user_action_callback,
                                    gpointer app_needs_user_action_data,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data)
{
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (plugin);
  g_autoptr(GTask) task = NULL;
  gboolean safe_mode = TRUE;

  task = g_task_new (plugin, cancellable, callback, user_data);
  g_task_set_source_tag (task, gs_plugin_aptkit_update_apps_async);

  if (flags & GS_PLUGIN_UPDATE_APPS_FLAGS_NO_APPLY) {
    g_task_return_boolean (task, TRUE);
    return;
  }

  if (self->tried_safe_mode) {
    /* If we've already tried safe mode and found no updates, turn it off */
    safe_mode = FALSE;
    g_debug ("Using non-safe mode for update since safe mode had no updates");
  }

  g_object_set_data (G_OBJECT (task), "safe-mode", GINT_TO_POINTER (safe_mode));

  gs_app_list_remove_all (self->updatable_apps);
  for (guint i = 0; i < gs_app_list_length (list); i++) {
    GsApp *app = gs_app_list_index (list, i);
    const gchar *package_name = gs_app_get_metadata_item (app, "aptkit::package-name");
    if (package_name != NULL) {
      g_debug ("Adding package to upgrade: %s", package_name);
      gs_app_list_add (self->updatable_apps, app);
      gs_app_set_state (app, GS_APP_STATE_INSTALLING);
    }
  }

  g_task_set_task_data (task, GINT_TO_POINTER (ACTION_UPGRADE_SYSTEM), NULL);

  g_debug ("Starting system update with safe mode %s", safe_mode ? "on" : "off");
  g_dbus_proxy_call (self->aptkit_proxy,
                     "UpgradeSystem",
                     g_variant_new ("(b)", safe_mode),
                     G_DBUS_CALL_FLAGS_NONE,
                     -1,
                     cancellable,
                     aptkit_upgrade_system_cb,
                     g_steal_pointer (&task));
}

static void
gs_plugin_aptkit_init (GsPluginAptkit *self)
{
  GsPlugin *plugin = GS_PLUGIN (self);

  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "icons");
  gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_BEFORE, "generic-updates");

  self->updatable_apps = gs_app_list_new ();
  self->tried_safe_mode = FALSE;
}

static void
gs_plugin_aptkit_dispose (GObject *object)
{
  GsPluginAptkit *self = GS_PLUGIN_APTKIT (object);

  g_clear_object (&self->aptkit_proxy);
  g_clear_object (&self->updatable_apps);

  G_OBJECT_CLASS (gs_plugin_aptkit_parent_class)->dispose (object);
}

static void
gs_plugin_aptkit_class_init (GsPluginAptkitClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GsPluginClass *plugin_class = GS_PLUGIN_CLASS (klass);

  object_class->dispose = gs_plugin_aptkit_dispose;

  plugin_class->setup_async = gs_plugin_aptkit_setup_async;
  plugin_class->setup_finish = gs_plugin_aptkit_setup_finish;
  plugin_class->refresh_metadata_async = gs_plugin_aptkit_refresh_metadata_async;
  plugin_class->refresh_metadata_finish = gs_plugin_aptkit_refresh_metadata_finish;
  plugin_class->list_apps_async = gs_plugin_aptkit_list_apps_async;
  plugin_class->list_apps_finish = gs_plugin_aptkit_list_apps_finish;
  plugin_class->launch_async = gs_plugin_aptkit_launch_async;
  plugin_class->launch_finish = gs_plugin_aptkit_launch_finish;
  plugin_class->update_apps_async = gs_plugin_aptkit_update_apps_async;
  plugin_class->update_apps_finish = gs_plugin_aptkit_update_apps_finish;
}

GType
gs_plugin_query_type (void)
{
  return GS_TYPE_PLUGIN_APTKIT;
}
