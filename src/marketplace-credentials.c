#include "marketplace-credentials.h"

#include <gmodule.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#if defined(G_OS_WIN32)
#include <windows.h>
#include <wincrypt.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

typedef enum {
  CREDENTIAL_LOAD,
  CREDENTIAL_STORE,
  CREDENTIAL_CLEAR,
} CredentialOperation;

typedef struct {
  CredentialOperation operation;
  char *token;
} CredentialRequest;

#if defined(__linux__)
typedef struct _PumpkinSecretSchema PumpkinSecretSchema;

typedef gchar *(*SecretPasswordLookupSync)(const PumpkinSecretSchema *schema,
                                           GCancellable *cancellable,
                                           GError **error,
                                           ...);
typedef gboolean (*SecretPasswordStoreSync)(const PumpkinSecretSchema *schema,
                                            const gchar *collection,
                                            const gchar *label,
                                            const gchar *password,
                                            GCancellable *cancellable,
                                            GError **error,
                                            ...);
typedef gboolean (*SecretPasswordClearSync)(const PumpkinSecretSchema *schema,
                                            GCancellable *cancellable,
                                            GError **error,
                                            ...);
typedef PumpkinSecretSchema *(*SecretSchemaNew)(const gchar *name,
                                                 guint flags,
                                                 ...);
typedef void (*SecretSchemaUnref)(PumpkinSecretSchema *schema);

typedef struct {
  GModule *module;
  PumpkinSecretSchema *schema;
  SecretPasswordLookupSync lookup;
  SecretPasswordStoreSync store;
  SecretPasswordClearSync clear;
  SecretSchemaNew schema_new;
  SecretSchemaUnref schema_unref;
} SecretApi;

static SecretApi secret_api;
static gsize secret_api_initialized;

static void
initialize_secret_api(void)
{
  if (!g_module_supported()) {
    return;
  }

  /* Keep libsecret optional so non-Linux builds do not acquire a Linux-only dependency. */
  secret_api.module = g_module_open("libsecret-1.so.0", G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  if (secret_api.module == NULL) {
    secret_api.module = g_module_open("libsecret-1.so", G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
  }
  if (secret_api.module == NULL ||
      !g_module_symbol(secret_api.module, "secret_password_lookup_sync", (gpointer *)&secret_api.lookup) ||
      !g_module_symbol(secret_api.module, "secret_password_store_sync", (gpointer *)&secret_api.store) ||
      !g_module_symbol(secret_api.module, "secret_password_clear_sync", (gpointer *)&secret_api.clear) ||
      !g_module_symbol(secret_api.module, "secret_schema_new", (gpointer *)&secret_api.schema_new) ||
      !g_module_symbol(secret_api.module, "secret_schema_unref", (gpointer *)&secret_api.schema_unref)) {
    g_clear_pointer(&secret_api.module, g_module_close);
    secret_api.lookup = NULL;
    secret_api.store = NULL;
    secret_api.clear = NULL;
    secret_api.schema_new = NULL;
    secret_api.schema_unref = NULL;
    return;
  }

  secret_api.schema = secret_api.schema_new("dev.rotstein.SmashedPumpkin.Marketplace", 0,
                                            "account", 0, NULL);
  if (secret_api.schema == NULL) {
    g_clear_pointer(&secret_api.module, g_module_close);
    secret_api.lookup = NULL;
    secret_api.store = NULL;
    secret_api.clear = NULL;
    secret_api.schema_new = NULL;
    secret_api.schema_unref = NULL;
  }
}

static gboolean
ensure_secret_api(GError **error)
{
  if (g_once_init_enter(&secret_api_initialized)) {
    initialize_secret_api();
    g_once_init_leave(&secret_api_initialized, 1);
  }
  if (secret_api.module != NULL && secret_api.schema != NULL) {
    return TRUE;
  }

  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "No Secret Service implementation is available");
  return FALSE;
}
#endif

#if defined(G_OS_WIN32)
static char *
windows_credential_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "smashed-pumpkin",
                          "marketplace-credential.bin", NULL);
}

static gboolean
windows_store_credential(const char *token, GError **error)
{
  DATA_BLOB input = { (DWORD)strlen(token), (BYTE *)token };
  DATA_BLOB encrypted = { 0, NULL };
  if (!CryptProtectData(&input, L"Smashed Pumpkin Marketplace API key", NULL, NULL, NULL,
                        CRYPTPROTECT_UI_FORBIDDEN, &encrypted)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Windows could not protect the Marketplace API key (%lu)", GetLastError());
    return FALSE;
  }

  g_autofree char *path = windows_credential_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autofree char *temporary_path = g_strconcat(path, ".tmp", NULL);
  gboolean saved = g_mkdir_with_parents(directory, 0700) == 0 &&
                   g_file_set_contents(temporary_path, (const char *)encrypted.pbData,
                                       encrypted.cbData, error) &&
                   g_rename(temporary_path, path) == 0;
  if (!saved && error != NULL && *error == NULL) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                "Could not save the protected Marketplace API key: %s", g_strerror(errno));
  }
  if (!saved) {
    g_remove(temporary_path);
  }
  LocalFree(encrypted.pbData);
  return saved;
}

static char *
windows_load_credential(GError **error)
{
  g_autofree char *path = windows_credential_path();
  g_autofree gchar *encrypted_data = NULL;
  gsize encrypted_length = 0;
  if (!g_file_get_contents(path, &encrypted_data, &encrypted_length, error)) {
    if (error != NULL && g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(error);
      return NULL;
    }
    return NULL;
  }

  DATA_BLOB encrypted = { (DWORD)encrypted_length, (BYTE *)encrypted_data };
  DATA_BLOB decrypted = { 0, NULL };
  if (!CryptUnprotectData(&encrypted, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &decrypted)) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Windows could not read the Marketplace API key (%lu)", GetLastError());
    return NULL;
  }

  char *token = g_strndup((const char *)decrypted.pbData, decrypted.cbData);
  LocalFree(decrypted.pbData);
  return token;
}

static gboolean
windows_clear_credential(GError **error)
{
  g_autofree char *path = windows_credential_path();
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    return TRUE;
  }
  if (g_remove(path) == 0) {
    return TRUE;
  }
  g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
              "Could not remove the protected Marketplace API key: %s", g_strerror(errno));
  return FALSE;
}
#elif defined(__APPLE__)
static CFMutableDictionaryRef
macos_credential_query(void)
{
  CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query, kSecAttrService, CFSTR("dev.rotstein.SmashedPumpkin"));
  CFDictionarySetValue(query, kSecAttrAccount, CFSTR("marketplace-api-key"));
  return query;
}

static void
set_macos_error(GError **error, const char *operation, OSStatus status)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
              "macOS Keychain could not %s the Marketplace API key (%d)", operation, (int)status);
}

static gboolean
macos_store_credential(const char *token, GError **error)
{
  CFMutableDictionaryRef query = macos_credential_query();
  CFDataRef value = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)token, (CFIndex)strlen(token));
  CFMutableDictionaryRef update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                             &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(update, kSecValueData, value);
  OSStatus status = SecItemUpdate(query, update);
  if (status == errSecItemNotFound) {
    CFDictionarySetValue(query, kSecValueData, value);
    status = SecItemAdd(query, NULL);
  }
  CFRelease(update);
  CFRelease(value);
  CFRelease(query);
  if (status == errSecSuccess) {
    return TRUE;
  }
  set_macos_error(error, "store", status);
  return FALSE;
}

static char *
macos_load_credential(GError **error)
{
  CFMutableDictionaryRef query = macos_credential_query();
  CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
  CFTypeRef result = NULL;
  OSStatus status = SecItemCopyMatching(query, &result);
  CFRelease(query);
  if (status == errSecItemNotFound) {
    return NULL;
  }
  if (status != errSecSuccess) {
    set_macos_error(error, "read", status);
    return NULL;
  }

  CFDataRef value = (CFDataRef)result;
  char *token = g_strndup((const char *)CFDataGetBytePtr(value), (gsize)CFDataGetLength(value));
  CFRelease(result);
  return token;
}

static gboolean
macos_clear_credential(GError **error)
{
  CFMutableDictionaryRef query = macos_credential_query();
  OSStatus status = SecItemDelete(query);
  CFRelease(query);
  if (status == errSecSuccess || status == errSecItemNotFound) {
    return TRUE;
  }
  set_macos_error(error, "remove", status);
  return FALSE;
}
#endif

static void
credential_request_free(CredentialRequest *request)
{
  if (request == NULL) {
    return;
  }
  if (request->token != NULL) {
    memset(request->token, 0, strlen(request->token));
  }
  g_free(request->token);
  g_free(request);
}

/* API keys leave no plaintext copies behind when freed. */
static void
secret_token_free(gpointer data)
{
  if (data != NULL) memset(data, 0, strlen(data));
  g_free(data);
}

static void
credential_task(GTask *task,
                gpointer source_object,
                gpointer task_data,
                GCancellable *cancellable)
{
  (void)source_object;
  CredentialRequest *request = task_data;

#if defined(__linux__)
  g_autoptr(GError) error = NULL;
  if (!ensure_secret_api(&error)) {
    g_task_return_error(task, g_steal_pointer(&error));
    return;
  }

  switch (request->operation) {
    case CREDENTIAL_LOAD: {
      char *token = secret_api.lookup(secret_api.schema, cancellable, &error,
                                      "account", "default", NULL);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_pointer(task, token, secret_token_free);
      }
      return;
    }
    case CREDENTIAL_STORE:
      if (secret_api.store(secret_api.schema, NULL,
                           "Smashed Pumpkin Marketplace API key", request->token,
                           cancellable, &error, "account", "default", NULL)) {
        g_task_return_boolean(task, TRUE);
      } else if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, FALSE);
      }
      return;
    case CREDENTIAL_CLEAR:
      if (secret_api.clear(secret_api.schema, cancellable, &error,
                           "account", "default", NULL)) {
        g_task_return_boolean(task, TRUE);
      } else if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, FALSE);
      }
      return;
  }
#elif defined(G_OS_WIN32)
  (void)cancellable;
  g_autoptr(GError) error = NULL;
  switch (request->operation) {
    case CREDENTIAL_LOAD: {
      char *token = windows_load_credential(&error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_pointer(task, token, secret_token_free);
      }
      break;
    }
    case CREDENTIAL_STORE: {
      gboolean stored = windows_store_credential(request->token, &error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, stored);
      }
      break;
    }
    case CREDENTIAL_CLEAR: {
      gboolean cleared = windows_clear_credential(&error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, cleared);
      }
      break;
    }
  }
  return;
#elif defined(__APPLE__)
  (void)cancellable;
  g_autoptr(GError) error = NULL;
  switch (request->operation) {
    case CREDENTIAL_LOAD: {
      char *token = macos_load_credential(&error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_pointer(task, token, secret_token_free);
      }
      break;
    }
    case CREDENTIAL_STORE: {
      gboolean stored = macos_store_credential(request->token, &error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, stored);
      }
      break;
    }
    case CREDENTIAL_CLEAR: {
      gboolean cleared = macos_clear_credential(&error);
      if (error != NULL) {
        g_task_return_error(task, g_steal_pointer(&error));
      } else {
        g_task_return_boolean(task, cleared);
      }
      break;
    }
  }
  return;
#else
  (void)cancellable;
#endif

  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                          "Secure credential storage is not available on this platform yet");
}

static void
run_credential_task(CredentialOperation operation,
                    const char *token,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
  CredentialRequest *request = g_new0(CredentialRequest, 1);
  request->operation = operation;
  request->token = g_strdup(token);

  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_task_data(task, request, (GDestroyNotify)credential_request_free);
  g_task_run_in_thread(task, credential_task);
  g_object_unref(task);
}

void
pumpkin_marketplace_credentials_load_async(GAsyncReadyCallback callback,
                                          gpointer user_data)
{
  run_credential_task(CREDENTIAL_LOAD, NULL, callback, user_data);
}

char *
pumpkin_marketplace_credentials_load_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
pumpkin_marketplace_credentials_store_async(const char *token,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data)
{
  run_credential_task(CREDENTIAL_STORE, token, callback, user_data);
}

gboolean
pumpkin_marketplace_credentials_store_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}

void
pumpkin_marketplace_credentials_clear_async(GAsyncReadyCallback callback,
                                           gpointer user_data)
{
  run_credential_task(CREDENTIAL_CLEAR, NULL, callback, user_data);
}

gboolean
pumpkin_marketplace_credentials_clear_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean(G_TASK(result), error);
}
