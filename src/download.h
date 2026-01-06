#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PumpkinDownload PumpkinDownload;

typedef void (*PumpkinDownloadProgress)(goffset current,
                                        goffset total,
                                        gpointer user_data);

typedef enum {
  PUMPKIN_DOWNLOAD_OK,
  PUMPKIN_DOWNLOAD_FALLBACK_USED
} PumpkinDownloadResult;

void pumpkin_download_file_async(const char *url,
                                 const char *dest_path,
                                 GCancellable *cancellable,
                                 PumpkinDownloadProgress progress_cb,
                                 gpointer progress_data,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

gboolean pumpkin_download_file_finish(GAsyncResult *result, GError **error);

void pumpkin_resolve_latest_async(GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);

char *pumpkin_resolve_latest_finish(GAsyncResult *result,
                                    PumpkinDownloadResult *result_code,
                                    GError **error);

G_END_DECLS
