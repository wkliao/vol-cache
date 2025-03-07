/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.  *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Purpose:     This is a "cache" VOL connector
 *
 */

/* Header files needed */
/* Do NOT include private HDF5 files here! */
#include <assert.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Public HDF5 files */
#include "hdf5.h"

/* Async VOL connector's header files */
#include "h5_async_lib.h"
#include "h5_async_vol.h"

/* This connector's header */
#include "mpi.h"
#include "unistd.h"
// POSIX I/O
#include <fcntl.h>

// Memory map
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
// debug
#include "debug.h"
// VOL related header
#include "H5LS.h"
#include "H5VLcache_ext_private.h"
#include "cache_new_h5api.h"
#include "cache_utils.h"
#ifndef TRUE
#define TRUE true
#endif
#include <dirent.h>
/**********/
/* Macros */
/**********/

/* Hack for missing va_copy() in old Visual Studio editions
 * (from H5win2_defs.h - used on VS2012 and earlier)
 */
#if defined(_WIN32) && defined(_MSC_VER) && (_MSC_VER < 1800)
#define va_copy(D, S) ((D) = (S))
#endif

#define PAGESIZE sysconf(_SC_PAGE_SIZE)
#ifndef SUCCEED
#define SUCCEED 0
#endif

#ifndef FAIL
#define FAIL -1
#endif

#ifndef H5_REQUEST_NULL
#define H5_REQUEST_NULL NULL
#endif

#define INF UINT64_MAX

#ifndef STDERR
#ifdef __APPLE__
#define STDERR __stderrp
#else
#define STDERR stderr
#endif
#endif

int RANK = 0;
int NPROC = 1;
hbool_t CLOSE_ASYNC = 0;
// Functions from async VOL
int H5VL_async_set_delay_time(uint64_t time_us);
herr_t H5VL_async_set_request_dep(void *request, void *parent_request);
herr_t H5async_start(void *request);
herr_t H5async_stop(void *request);
herr_t H5VL_async_pause();
herr_t H5VL_async_start();
/************/
/* Typedefs */
/************/

/* The cache VOL wrapper context */
typedef struct H5VL_cache_ext_wrap_ctx_t {
  hid_t under_vol_id;   /* VOL ID for under VOL */
  void *under_wrap_ctx; /* Object wrapping context for under VOL */
} H5VL_cache_ext_wrap_ctx_t;

/********************* */
/* Function prototypes */
/********************* */
/* Helper routines */
static H5VL_cache_ext_t *H5VL_cache_ext_new_obj(void *under_obj,
                                                hid_t under_vol_id);
static herr_t H5VL_cache_ext_free_obj(H5VL_cache_ext_t *obj);

/* "Management" callbacks */
static herr_t H5VL_cache_ext_init(hid_t vipl_id);
static herr_t H5VL_cache_ext_term(void);

/* VOL info callbacks */
static void *H5VL_cache_ext_info_copy(const void *info);
static herr_t H5VL_cache_ext_info_cmp(int *cmp_value, const void *info1,
                                      const void *info2);
static herr_t H5VL_cache_ext_info_free(void *info);
static herr_t H5VL_cache_ext_info_to_str(const void *info, char **str);
static herr_t H5VL_cache_ext_str_to_info(const char *str, void **info);

/* VOL object wrap / retrieval callbacks */
static void *H5VL_cache_ext_get_object(const void *obj);
static herr_t H5VL_cache_ext_get_wrap_ctx(const void *obj, void **wrap_ctx);
static void *H5VL_cache_ext_wrap_object(void *obj, H5I_type_t obj_type,
                                        void *wrap_ctx);
static void *H5VL_cache_ext_unwrap_object(void *obj);
static herr_t H5VL_cache_ext_free_wrap_ctx(void *obj);

/* Attribute callbacks */
static void *H5VL_cache_ext_attr_create(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        const char *name, hid_t type_id,
                                        hid_t space_id, hid_t acpl_id,
                                        hid_t aapl_id, hid_t dxpl_id,
                                        void **req);
static void *H5VL_cache_ext_attr_open(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      const char *name, hid_t aapl_id,
                                      hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_attr_read(void *attr, hid_t mem_type_id, void *buf,
                                       hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_attr_write(void *attr, hid_t mem_type_id,
                                        const void *buf, hid_t dxpl_id,
                                        void **req);
static herr_t H5VL_cache_ext_attr_get(void *obj, H5VL_attr_get_args_t *args,
                                      hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_attr_specific(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_attr_specific_args_t *args,
                                           hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_attr_optional(void *obj,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_attr_close(void *attr, hid_t dxpl_id, void **req);

/* Dataset callbacks */
static void *H5VL_cache_ext_dataset_create(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           const char *name, hid_t lcpl_id,
                                           hid_t type_id, hid_t space_id,
                                           hid_t dcpl_id, hid_t dapl_id,
                                           hid_t dxpl_id, void **req);
static void *H5VL_cache_ext_dataset_open(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         const char *name, hid_t dapl_id,
                                         hid_t dxpl_id, void **req);
#if H5_VERSION_GE(1, 13, 3)
static herr_t H5VL_cache_ext_dataset_read(size_t count, void *dset[],
                                          hid_t mem_type_id[],
                                          hid_t mem_space_id[],
                                          hid_t file_space_id[], hid_t plist_id,
                                          void *buf[], void **req);

static herr_t
H5VL_cache_ext_dataset_write(size_t count, void *dset[], hid_t mem_type_id[],
                             hid_t mem_space_id[], hid_t file_space_id[],
                             hid_t plist_id, const void *buf[], void **req);
static herr_t H5VL_cache_ext_dataset_read_from_cache(
    size_t count, void *dset[], hid_t mem_type_id[], hid_t mem_space_id[],
    hid_t file_space_id[], hid_t plist_id, void *buf[], void **req);
static herr_t H5VL_cache_ext_dataset_read_to_cache(
    size_t count, void *dset[], hid_t mem_type_id[], hid_t mem_space_id[],
    hid_t file_space_id[], hid_t plist_id, void *buf[], void **req);
static herr_t H5VL_cache_ext_dataset_prefetch(void *dset, hid_t file_type_id,
                                              hid_t plist_id, void **req);
#else
static herr_t H5VL_cache_ext_dataset_read(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          void *buf, void **req);
static herr_t
H5VL_cache_ext_dataset_read_from_cache(void *dset, hid_t mem_type_id,
                                       hid_t mem_space_id, hid_t file_space_id,
                                       hid_t plist_id, void *buf, void **req);
static herr_t
H5VL_cache_ext_dataset_read_to_cache(void *dset, hid_t mem_type_id,
                                     hid_t mem_space_id, hid_t file_space_id,
                                     hid_t plist_id, void *buf, void **req);
static herr_t H5VL_cache_ext_dataset_prefetch(void *dset, hid_t file_type_id,
                                              hid_t plist_id, void **req);

static herr_t H5VL_cache_ext_dataset_write(void *dset, hid_t mem_type_id,
                                           hid_t mem_space_id,
                                           hid_t file_space_id, hid_t plist_id,
                                           const void *buf, void **req);
#endif

static herr_t H5VL_cache_ext_dataset_get(void *dset,
                                         H5VL_dataset_get_args_t *args,
                                         hid_t dxpl_id, void **req);
static herr_t
H5VL_cache_ext_dataset_specific(void *obj, H5VL_dataset_specific_args_t *args,
                                hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_dataset_optional(void *obj,
                                              H5VL_optional_args_t *args,
                                              hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_dataset_close(void *dset, hid_t dxpl_id,
                                           void **req);

/* Datatype callbacks */
static void *H5VL_cache_ext_datatype_commit(void *obj,
                                            const H5VL_loc_params_t *loc_params,
                                            const char *name, hid_t type_id,
                                            hid_t lcpl_id, hid_t tcpl_id,
                                            hid_t tapl_id, hid_t dxpl_id,
                                            void **req);
static void *H5VL_cache_ext_datatype_open(void *obj,
                                          const H5VL_loc_params_t *loc_params,
                                          const char *name, hid_t tapl_id,
                                          hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_datatype_get(void *dt,
                                          H5VL_datatype_get_args_t *args,
                                          hid_t dxpl_id, void **req);
static herr_t
H5VL_cache_ext_datatype_specific(void *obj, H5VL_datatype_specific_args_t *args,
                                 hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_datatype_optional(void *obj,
                                               H5VL_optional_args_t *args,
                                               hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_datatype_close(void *dt, hid_t dxpl_id,
                                            void **req);

/* File callbacks */
static void *H5VL_cache_ext_file_create(const char *name, unsigned flags,
                                        hid_t fcpl_id, hid_t fapl_id,
                                        hid_t dxpl_id, void **req);
static void *H5VL_cache_ext_file_open(const char *name, unsigned flags,
                                      hid_t fapl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_file_get(void *file, H5VL_file_get_args_t *args,
                                      hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_file_specific(void *file,
                                           H5VL_file_specific_args_t *args,
                                           hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_file_optional(void *file,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_file_close(void *file, hid_t dxpl_id, void **req);

/* Group callbacks */
static void *H5VL_cache_ext_group_create(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         const char *name, hid_t lcpl_id,
                                         hid_t gcpl_id, hid_t gapl_id,
                                         hid_t dxpl_id, void **req);
static void *H5VL_cache_ext_group_open(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       const char *name, hid_t gapl_id,
                                       hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_group_get(void *obj, H5VL_group_get_args_t *args,
                                       hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_group_specific(void *obj,
                                            H5VL_group_specific_args_t *args,
                                            hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_group_optional(void *obj,
                                            H5VL_optional_args_t *args,
                                            hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_group_close(void *grp, hid_t dxpl_id, void **req);

/* Link callbacks */
static herr_t H5VL_cache_ext_link_create(H5VL_link_create_args_t *args,
                                         void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         hid_t lcpl_id, hid_t lapl_id,
                                         hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_link_copy(void *src_obj,
                                       const H5VL_loc_params_t *loc_params1,
                                       void *dst_obj,
                                       const H5VL_loc_params_t *loc_params2,
                                       hid_t lcpl_id, hid_t lapl_id,
                                       hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_link_move(void *src_obj,
                                       const H5VL_loc_params_t *loc_params1,
                                       void *dst_obj,
                                       const H5VL_loc_params_t *loc_params2,
                                       hid_t lcpl_id, hid_t lapl_id,
                                       hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_link_get(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      H5VL_link_get_args_t *args, hid_t dxpl_id,
                                      void **req);
static herr_t H5VL_cache_ext_link_specific(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_link_specific_args_t *args,
                                           hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_link_optional(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req);

/* Object callbacks */
static void *H5VL_cache_ext_object_open(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        H5I_type_t *opened_type, hid_t dxpl_id,
                                        void **req);
static herr_t H5VL_cache_ext_object_copy(
    void *src_obj, const H5VL_loc_params_t *src_loc_params,
    const char *src_name, void *dst_obj,
    const H5VL_loc_params_t *dst_loc_params, const char *dst_name,
    hid_t ocpypl_id, hid_t lcpl_id, hid_t dxpl_id, void **req);
static herr_t H5VL_cache_ext_object_get(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        H5VL_object_get_args_t *args,
                                        hid_t dxpl_id, void **req);
static herr_t
H5VL_cache_ext_object_specific(void *obj, const H5VL_loc_params_t *loc_params,
                               H5VL_object_specific_args_t *args, hid_t dxpl_id,
                               void **req);
static herr_t
H5VL_cache_ext_object_optional(void *obj, const H5VL_loc_params_t *loc_params,
                               H5VL_optional_args_t *args, hid_t dxpl_id,
                               void **req);

/* Container/connector introspection callbacks */
static herr_t
H5VL_cache_ext_introspect_get_conn_cls(void *obj, H5VL_get_conn_lvl_t lvl,
                                       const H5VL_class_t **conn_cls);
static herr_t H5VL_cache_ext_introspect_get_cap_flags(const void *info,
                                                      unsigned *cap_flags);
static herr_t H5VL_cache_ext_introspect_opt_query(void *obj,
                                                  H5VL_subclass_t cls,
                                                  int opt_type,
                                                  uint64_t *flags);

/* Async request callbacks */
static herr_t H5VL_cache_ext_request_wait(void *req, uint64_t timeout,
                                          H5VL_request_status_t *status);
static herr_t H5VL_cache_ext_request_notify(void *obj, H5VL_request_notify_t cb,
                                            void *ctx);
static herr_t H5VL_cache_ext_request_cancel(void *req,
                                            H5VL_request_status_t *status);
static herr_t
H5VL_cache_ext_request_specific(void *req, H5VL_request_specific_args_t *args);
static herr_t H5VL_cache_ext_request_optional(void *req,
                                              H5VL_optional_args_t *args);
static herr_t H5VL_cache_ext_request_free(void *req);

/* Blob callbacks */
static herr_t H5VL_cache_ext_blob_put(void *obj, const void *buf, size_t size,
                                      void *blob_id, void *ctx);
static herr_t H5VL_cache_ext_blob_get(void *obj, const void *blob_id, void *buf,
                                      size_t size, void *ctx);
static herr_t H5VL_cache_ext_blob_specific(void *obj, void *blob_id,
                                           H5VL_blob_specific_args_t *args);
static herr_t H5VL_cache_ext_blob_optional(void *obj, void *blob_id,
                                           H5VL_optional_args_t *args);

/* Token callbacks */
static herr_t H5VL_cache_ext_token_cmp(void *obj, const H5O_token_t *token1,
                                       const H5O_token_t *token2,
                                       int *cmp_value);
static herr_t H5VL_cache_ext_token_to_str(void *obj, H5I_type_t obj_type,
                                          const H5O_token_t *token,
                                          char **token_str);
static herr_t H5VL_cache_ext_token_from_str(void *obj, H5I_type_t obj_type,
                                            const char *token_str,
                                            H5O_token_t *token);

/* Generic optional callback */
static herr_t H5VL_cache_ext_optional(void *obj, H5VL_optional_args_t *args,
                                      hid_t dxpl_id, void **req);

/* The cache VOL local function */
static herr_t H5VL_cache_ext_dataset_wait(void *o);
static herr_t H5VL_cache_ext_file_wait(void *o);

static herr_t create_file_cache_on_local_storage(void *obj, void *file_args,
                                                 void **req);
static herr_t create_group_cache_on_local_storage(void *obj, void *group_args,
                                                  void **req);
static herr_t create_dataset_cache_on_local_storage(void *obj, void *dset_args,
                                                    void **req);

static herr_t remove_file_cache_on_local_storage(void *obj, void **req);
static herr_t remove_group_cache_on_local_storage(void *obj, void **req);
static herr_t remove_dataset_cache_on_local_storage(void *obj, void **req);
static void *write_data_to_local_storage(void *dset, hid_t mem_type_id,
                                         hid_t mem_space_id,
                                         hid_t file_space_id, hid_t plist_id,
                                         const void *buf, void **req);
// currently because read and write are not unified, I have to use two different
// functions of write_data_to_cache
static void *write_data_to_local_storage2(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          const void *buf, void **req);

static herr_t read_data_from_local_storage(void *dset, hid_t mem_type_id,
                                           hid_t mem_space_id,
                                           hid_t file_space_id, hid_t plist_id,
                                           void *buf, void **req);
#if H5_VERSION_GE(1, 13, 3)
static herr_t flush_data_from_local_storage(size_t count, void *dset[],
                                            void **req);
#else
static herr_t flush_data_from_local_storage(void *dset, void **req);
#endif
static herr_t create_file_cache_on_global_storage(void *obj, void *file_args,
                                                  void **req);
static herr_t create_group_cache_on_global_storage(void *obj, void *group_args,
                                                   void **req);
static herr_t create_dataset_cache_on_global_storage(void *obj, void *dset_args,
                                                     void **req);
static herr_t remove_file_cache_on_global_storage(void *obj, void **req);
static herr_t remove_group_cache_on_global_storage(void *obj, void **req);
static herr_t remove_dataset_cache_on_global_storage(void *obj, void **req);
static void *write_data_to_global_storage(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          const void *buf, void **req);
// currently because read and write are not unified, I have to use two different
// functions of write_data_to_cache

static herr_t read_data_from_global_storage(void *dset, hid_t mem_type_id,
                                            hid_t mem_space_id,
                                            hid_t file_space_id, hid_t plist_id,
                                            void *buf, void **req);
#if H5_VERSION_GE(1, 13, 3)
static herr_t flush_data_from_global_storage(size_t count, void *dset[],
                                             void **req);
#else
static herr_t flush_data_from_global_storage(void *dset, void **req);
#endif

static const H5LS_cache_io_class_t H5LS_cache_io_class_global_g = {
    "GLOBAL",
    create_file_cache_on_global_storage,    // create_file_cache
    remove_file_cache_on_global_storage,    // remove_file_cache
    create_group_cache_on_global_storage,   // create_group cache
    remove_group_cache_on_global_storage,   // remove_group cache
    create_dataset_cache_on_global_storage, // create_dataset_cache
    remove_dataset_cache_on_global_storage, // remove_dataset_cache
    write_data_to_global_storage,           // write_data_to_cache
    write_data_to_global_storage,           // write_data_to_cache
    flush_data_from_global_storage,         // flush_data_from_cache
    read_data_from_global_storage,          // read_data_from_cache
};

static const H5LS_cache_io_class_t H5LS_cache_io_class_local_g = {
    "LOCAL",
    create_file_cache_on_local_storage,
    remove_file_cache_on_local_storage,
    create_group_cache_on_local_storage,
    remove_group_cache_on_local_storage,
    create_dataset_cache_on_local_storage,
    remove_dataset_cache_on_local_storage,
    write_data_to_local_storage,
    write_data_to_local_storage2,
    flush_data_from_local_storage,
    read_data_from_local_storage,
};

/*******************/
/* Local variables */
/*******************/

/* Cache VOL connector class struct */
static const H5VL_class_t H5VL_cache_ext_g = {
    H5VL_VERSION,                             /* version      */
    (H5VL_class_value_t)H5VL_CACHE_EXT_VALUE, /* value        */
    H5VL_CACHE_EXT_NAME,                      /* name         */
    H5VL_CACHE_EXT_VERSION,                   /* connector version */
    0,                                        /* capability flags */
    H5VL_cache_ext_init,                      /* initialize   */
    H5VL_cache_ext_term,                      /* terminate    */
    {
        /* info_cls */
        sizeof(H5VL_cache_ext_info_t), /* size    */
        H5VL_cache_ext_info_copy,      /* copy    */
        H5VL_cache_ext_info_cmp,       /* compare */
        H5VL_cache_ext_info_free,      /* free    */
        H5VL_cache_ext_info_to_str,    /* to_str  */
        H5VL_cache_ext_str_to_info     /* from_str */
    },
    {
        /* wrap_cls */
        H5VL_cache_ext_get_object,    /* get_object   */
        H5VL_cache_ext_get_wrap_ctx,  /* get_wrap_ctx */
        H5VL_cache_ext_wrap_object,   /* wrap_object  */
        H5VL_cache_ext_unwrap_object, /* unwrap_object */
        H5VL_cache_ext_free_wrap_ctx  /* free_wrap_ctx */
    },
    {
        /* attribute_cls */
        H5VL_cache_ext_attr_create,   /* create */
        H5VL_cache_ext_attr_open,     /* open */
        H5VL_cache_ext_attr_read,     /* read */
        H5VL_cache_ext_attr_write,    /* write */
        H5VL_cache_ext_attr_get,      /* get */
        H5VL_cache_ext_attr_specific, /* specific */
        H5VL_cache_ext_attr_optional, /* optional */
        H5VL_cache_ext_attr_close     /* close */
    },
    {
        /* dataset_cls */
        H5VL_cache_ext_dataset_create,   /* create */
        H5VL_cache_ext_dataset_open,     /* open */
        H5VL_cache_ext_dataset_read,     /* read */
        H5VL_cache_ext_dataset_write,    /* write */
        H5VL_cache_ext_dataset_get,      /* get */
        H5VL_cache_ext_dataset_specific, /* specific */
        H5VL_cache_ext_dataset_optional, /* optional */
        H5VL_cache_ext_dataset_close     /* close */
    },
    {
        /* datatype_cls */
        H5VL_cache_ext_datatype_commit,   /* commit */
        H5VL_cache_ext_datatype_open,     /* open */
        H5VL_cache_ext_datatype_get,      /* get_size */
        H5VL_cache_ext_datatype_specific, /* specific */
        H5VL_cache_ext_datatype_optional, /* optional */
        H5VL_cache_ext_datatype_close     /* close */
    },
    {
        /* file_cls */
        H5VL_cache_ext_file_create,   /* create */
        H5VL_cache_ext_file_open,     /* open */
        H5VL_cache_ext_file_get,      /* get */
        H5VL_cache_ext_file_specific, /* specific */
        H5VL_cache_ext_file_optional, /* optional */
        H5VL_cache_ext_file_close     /* close */
    },
    {
        /* group_cls */
        H5VL_cache_ext_group_create,   /* create */
        H5VL_cache_ext_group_open,     /* open */
        H5VL_cache_ext_group_get,      /* get */
        H5VL_cache_ext_group_specific, /* specific */
        H5VL_cache_ext_group_optional, /* optional */
        H5VL_cache_ext_group_close     /* close */
    },
    {
        /* link_cls */
        H5VL_cache_ext_link_create,   /* create */
        H5VL_cache_ext_link_copy,     /* copy */
        H5VL_cache_ext_link_move,     /* move */
        H5VL_cache_ext_link_get,      /* get */
        H5VL_cache_ext_link_specific, /* specific */
        H5VL_cache_ext_link_optional  /* optional */
    },
    {
        /* object_cls */
        H5VL_cache_ext_object_open,     /* open */
        H5VL_cache_ext_object_copy,     /* copy */
        H5VL_cache_ext_object_get,      /* get */
        H5VL_cache_ext_object_specific, /* specific */
        H5VL_cache_ext_object_optional  /* optional */
    },
    {
        /* introspect_cls */
        H5VL_cache_ext_introspect_get_conn_cls, /* get_conn_cls */
        H5VL_cache_ext_introspect_get_cap_flags,
        H5VL_cache_ext_introspect_opt_query, /* opt_query */
    },
    {
        /* request_cls */
        H5VL_cache_ext_request_wait,     /* wait */
        H5VL_cache_ext_request_notify,   /* notify */
        H5VL_cache_ext_request_cancel,   /* cancel */
        H5VL_cache_ext_request_specific, /* specific */
        H5VL_cache_ext_request_optional, /* optional */
        H5VL_cache_ext_request_free      /* free */
    },
    {
        /* blob_cls */
        H5VL_cache_ext_blob_put,      /* put */
        H5VL_cache_ext_blob_get,      /* get */
        H5VL_cache_ext_blob_specific, /* specific */
        H5VL_cache_ext_blob_optional  /* optional */
    },
    {
        /* token_cls */
        H5VL_cache_ext_token_cmp,     /* cmp */
        H5VL_cache_ext_token_to_str,  /* to_str */
        H5VL_cache_ext_token_from_str /* from_str */
    },
    H5VL_cache_ext_optional /* optional */
};
/* The connector identification number, initialized at runtime */
static hid_t H5VL_CACHE_EXT_g = H5I_INVALID_HID;
/* Operation values for new "API" routines */
/* These are initialized in the VOL connector's 'init' callback at runtime.
 *      It's good practice to reset them back to -1 in the 'term' callback.
 */

static int H5VL_cache_dataset_prefetch_op_g = -1;
static int H5VL_cache_dataset_read_to_cache_op_g = -1;
static int H5VL_cache_dataset_read_from_cache_op_g = -1;
static int H5VL_cache_dataset_mmap_remap_op_g = -1;

static int H5VL_cache_dataset_cache_create_op_g = -1;
static int H5VL_cache_dataset_cache_remove_op_g = -1;

static int H5VL_cache_dataset_cache_async_op_pause_op_g = -1; //
static int H5VL_cache_dataset_cache_async_op_start_op_g = -1; //

static int H5VL_cache_file_cache_create_op_g =
    -1; // this is for reserving cache space for the file
static int H5VL_cache_file_cache_remove_op_g = -1;         //
static int H5VL_cache_file_cache_async_op_pause_op_g = -1; //
static int H5VL_cache_file_cache_async_op_start_op_g = -1; //

static int H5VL_cache_file_cache_async_close_wait_op_g = -1; //
static int H5VL_cache_file_cache_async_close_set_op_g = -1;  //

static object_close_task_t *async_close_task_list, *async_close_task_current;

/* Cache Storage link variable */
typedef struct _H5LS_stack_t {
  char fconfig[255];          // configure name
  cache_storage_t *H5LS;      // local storage construct
  struct _H5LS_stack_t *next; // link
} H5LS_stack_t;
H5LS_stack_t *H5LS_stack;

typedef struct dset_args_t {
  void *obj;
  const H5VL_loc_params_t *loc_params;
  const char *name;
  hid_t lcpl_id;
  hid_t type_id;
  hid_t space_id;
  hid_t dcpl_id;
  hid_t dapl_id;
  hid_t dxpl_id;
} dset_args_t;

typedef struct file_args_t {
  const char *name;
  unsigned flags;
  hid_t fcpl_id;
  hid_t fapl_id;
  hid_t dxpl_id;
} file_args_t;

typedef struct group_args_t {
  const H5VL_loc_params_t *loc_params;
  const char *name;
  hid_t lcpl_id;
  hid_t gcpl_id;
  hid_t gapl_id;
  hid_t dxpl_id;
} group_args_t;

/* Get the cache_storage_t object for current VOL layer based on info object */
static cache_storage_t *get_cache_storage_obj(H5VL_cache_ext_info_t *info) {
  H5LS_stack_t *p = H5LS_stack;
  while (strcmp(p->fconfig, info->fconfig) && (p != NULL)) {
    p = p->next;
  }
  if (p != NULL)
    return p->H5LS;
  else
    return NULL;
}

/* Required shim routines, to enable dynamic loading of shared library */
/* The HDF5 library _must_ find routines with these names and signatures
 *      for a shared library that contains a VOL connector to be detected
 *      and loaded at runtime.
 */
H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_VOL; }
const void *H5PLget_plugin_info(void) { return &H5VL_cache_ext_g; }

/* LOG function for printing debug infomation */
static void LOG(int rank, const char *str) {
#ifndef NDEBUG
  if (debug_level() > 0)
    if (rank >= 0)
      printf(" [CACHE VOL: %d] %s\n", rank, str);
    else if (RANK == io_node())
      printf(" [CACHE VOL] %s\n", str);
#endif
}

herr_t set_close_async(hbool_t t) {
  CLOSE_ASYNC = t;
  return 0;
}
static herr_t async_close_task_wait(object_close_task_t *task) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)task->obj;
  H5VL_request_status_t status;
  H5VL_class_value_t under_value;
  H5VLget_value(o->under_vol_id, &under_value);
  if (under_value != H5VL_ASYNC_VALUE) {
    if (RANK == io_node())
      printf(" [CACHE VOL] **WARNING: Do not have Async VOL underneath it. "
             "Close is not async.");
    return 0;
  }
#ifndef NDEBUG
  double t0 = MPI_Wtime();
#endif
  H5VLrequest_wait(task->req, o->under_vol_id, UINT64_MAX, &status);
#ifndef NDEBUG
  double t1 = MPI_Wtime();
  if (RANK == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] delay close object: %d - time: %10.6f\n", task->type,
           t1 - t0);
#endif
  if (o->read_cache || o->write_cache)
    if (task->type == FILE_CLOSE)
      o->H5LS->cache_io_cls->remove_file_cache(task->obj, NULL);
    else if (task->type == GROUP_CLOSE)
      o->H5LS->cache_io_cls->remove_group_cache(task->obj, NULL);
    else if (task->type == DATASET_CLOSE)
      o->H5LS->cache_io_cls->remove_dataset_cache(task->obj, NULL);
  H5VL_cache_ext_free_obj(o);
#ifndef NDEBUG
  double t2 = MPI_Wtime();
  if (RANK == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] remove cache time: %10.6f\n", t2 - t1);
#endif
  return 0;
}

herr_t async_close_wait() {
  while (async_close_task_current->next != NULL) {
    async_close_task_wait(async_close_task_current);
    async_close_task_current = async_close_task_current->next;
  }
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_new_obj
 *
 * Purpose:     Create a new pass through object for an underlying object
 *
 * Return:      Success:    Pointer to the new pass through object
 *              Failure:    NULL
 *
 * Programmer:  Quincey Koziol
 *              Monday, December 3, 2018
 *
 *-------------------------------------------------------------------------
 */
static H5VL_cache_ext_t *H5VL_cache_ext_new_obj(void *under_obj,
                                                hid_t under_vol_id) {
  H5VL_cache_ext_t *new_obj;

  new_obj = (H5VL_cache_ext_t *)calloc(1, sizeof(H5VL_cache_ext_t));
  new_obj->under_object = under_obj;
  new_obj->under_vol_id = under_vol_id;
  H5Iinc_ref(new_obj->under_vol_id);

  return new_obj;
} /* end H5VL_cache_ext_new_obj() */

/*-------------------------------------------------------------------------
 * Function:    H5VL__cache_free_obj
 *
 * Purpose:     Release a pass through object
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 * Programmer:  Quincey Koziol
 *              Monday, December 3, 2018
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_free_obj(H5VL_cache_ext_t *obj) {
  hid_t err_id;

  err_id = H5Eget_current_stack();

  H5Idec_ref(obj->under_vol_id);

  H5Eset_current_stack(err_id);

  free(obj);

  return 0;
} /* end H5VL_cache_extfree_obj() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_register
 *
 * Purpose:     Register the pass-through VOL connector and retrieve an ID
 *              for it.
 *
 * Return:      Success:    The ID for the pass-through VOL connector
 *              Failure:    -1
 *
 * Programmer:  Quincey Koziol
 *              Wednesday, November 28, 2018
 *
 *-------------------------------------------------------------------------
 */
hid_t H5VL_cache_ext_register(void) {
  /* Singleton register the pass-through VOL connector ID */
  if (H5VL_CACHE_EXT_g < 0)
    H5VL_CACHE_EXT_g = H5VLregister_connector(&H5VL_cache_ext_g, H5P_DEFAULT);

  return H5VL_CACHE_EXT_g;
} /* end H5VL_cache_ext_register() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_init
 *
 * Purpose:     Initialize this VOL connector, performing any necessary
 *              operations for the connector that will apply to all containers
 *              accessed with the connector.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_init(hid_t vipl_id) {
  int rank;
  int provided;
  int called = 0;
  MPI_Initialized(&called);
  if (called == 1) {
    MPI_Comm_size(MPI_COMM_WORLD, &NPROC);
    MPI_Comm_rank(MPI_COMM_WORLD, &RANK);
  } else {
    int provided = 0;
    MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
    MPI_Comm_size(MPI_COMM_WORLD, &NPROC);
    MPI_Comm_rank(MPI_COMM_WORLD, &RANK);
  }
#ifndef NDEBUG
  if (debug_level() > 1 && RANK == io_node())
    printf(" [CACHE VOL] %s:%d: cache VOL is called.\n", __func__, __LINE__);
  if (RANK == io_node() && log_level() > 1)
    printf("------- EXT CACHE VOL INIT\n");
#endif

  /* Shut compiler up about unused parameter */
  vipl_id = vipl_id;

  assert(-1 == H5VL_cache_dataset_read_to_cache_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DREAD_TO_CACHE,
                                 &H5VL_cache_dataset_read_to_cache_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_prefetch_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DPREFETCH,
                                 &H5VL_cache_dataset_prefetch_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_read_from_cache_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DREAD_FROM_CACHE,
                                 &H5VL_cache_dataset_read_from_cache_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_cache_remove_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DCACHE_REMOVE,
                                 &H5VL_cache_dataset_cache_remove_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_cache_create_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DCACHE_CREATE,
                                 &H5VL_cache_dataset_cache_create_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_mmap_remap_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_DATASET,
                                 H5VL_CACHE_EXT_DYN_DMMAP_REMAP,
                                 &H5VL_cache_dataset_mmap_remap_op_g) < 0)
    return -1;

  assert(-1 == H5VL_cache_file_cache_remove_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE,
                                 H5VL_CACHE_EXT_DYN_FCACHE_REMOVE,
                                 &H5VL_cache_file_cache_remove_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_file_cache_create_op_g);
  if (H5VLregister_opt_operation(H5VL_SUBCLS_FILE,
                                 H5VL_CACHE_EXT_DYN_FCACHE_CREATE,
                                 &H5VL_cache_file_cache_create_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_file_cache_async_close_set_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_FILE, H5VL_CACHE_EXT_DYN_FCACHE_ASYNC_CLOSE_SET,
          &H5VL_cache_file_cache_async_close_set_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_file_cache_async_close_wait_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_FILE, H5VL_CACHE_EXT_DYN_FCACHE_ASYNC_CLOSE_WAIT,
          &H5VL_cache_file_cache_async_close_wait_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_file_cache_async_op_pause_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_FILE, H5VL_CACHE_EXT_DYN_FCACHE_ASYNC_OP_PAUSE,
          &H5VL_cache_file_cache_async_op_pause_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_file_cache_async_op_start_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_FILE, H5VL_CACHE_EXT_DYN_FCACHE_ASYNC_OP_START,
          &H5VL_cache_file_cache_async_op_start_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_cache_async_op_pause_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_DATASET, H5VL_CACHE_EXT_DYN_DCACHE_ASYNC_OP_PAUSE,
          &H5VL_cache_dataset_cache_async_op_pause_op_g) < 0)
    return (-1);

  assert(-1 == H5VL_cache_dataset_cache_async_op_start_op_g);
  if (H5VLregister_opt_operation(
          H5VL_SUBCLS_DATASET, H5VL_CACHE_EXT_DYN_DCACHE_ASYNC_OP_START,
          &H5VL_cache_dataset_cache_async_op_start_op_g) < 0)
    return (-1);

  // Initialize local storage struct, create the first one
  H5LS_stack = (H5LS_stack_t *)malloc(sizeof(H5LS_stack_t));
  H5LS_stack->next = NULL;
  if (!getenv("ABT_THREAD_STACKSIZE"))
    setenv("ABT_THREAD_STACKSIZE", "100000", 1);
  //  async_close_task_list = (object_close_task_t *)
  //  malloc(sizeof(object_close_task_t)); async_close_task_list->next = NULL;
  // async_close_task_current = async_close_task_list;
  /* Delay the async execution */
  return 0;
} /* end H5VL_cache_ext_init() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_term
 *
 * Purpose:     Terminate this VOL connector, performing any necessary
 *              operations for the connector that release connector-wide
 *              resources (usually created / initialized with the 'init'
 *              callback).
 *
 * Return:      Success:    0
 *              Failure:    (Can't fail)
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_term(void) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL TERM\n");
#endif

  /* Reset VOL ID */
  H5VL_CACHE_EXT_g = H5I_INVALID_HID;

  /* Reset operation values for new "API" routines */
  assert(-1 != H5VL_cache_dataset_read_to_cache_op_g);
  H5VL_cache_dataset_read_to_cache_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_prefetch_op_g);
  H5VL_cache_dataset_prefetch_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_read_from_cache_op_g);
  H5VL_cache_dataset_read_from_cache_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_cache_remove_op_g);
  H5VL_cache_dataset_cache_remove_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_cache_create_op_g);
  H5VL_cache_dataset_cache_create_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_mmap_remap_op_g);
  H5VL_cache_dataset_mmap_remap_op_g = (-1);

  assert(-1 != H5VL_cache_file_cache_create_op_g);
  H5VL_cache_file_cache_create_op_g = (-1);

  assert(-1 != H5VL_cache_file_cache_async_op_start_op_g);
  H5VL_cache_file_cache_async_op_start_op_g = (-1);

  assert(-1 != H5VL_cache_file_cache_async_op_pause_op_g);
  H5VL_cache_file_cache_async_op_pause_op_g = (-1);

  assert(-1 != H5VL_cache_file_cache_async_close_wait_op_g);
  H5VL_cache_file_cache_async_close_wait_op_g = (-1);

  assert(-1 != H5VL_cache_file_cache_async_close_set_op_g);
  H5VL_cache_file_cache_async_close_set_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_cache_async_op_start_op_g);
  H5VL_cache_dataset_cache_async_op_start_op_g = (-1);

  assert(-1 != H5VL_cache_dataset_cache_async_op_pause_op_g);
  H5VL_cache_dataset_cache_async_op_pause_op_g = (-1);

  free(H5LS_stack);
  H5LS_stack = NULL;

  // async_close_wait();// close all the objects if it hasn't been closed
  // already. free(async_close_task_list);

  return 0;
} /* end H5VL_cache_ext_term() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_info_copy
 *
 * Purpose:     Duplicate the connector's info object.
 *
 * Returns:     Success:    New connector info object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_info_copy(const void *_info) {
  const H5VL_cache_ext_info_t *info = (const H5VL_cache_ext_info_t *)_info;
  H5VL_cache_ext_info_t *new_info;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INFO Copy\n");
#endif

  /* Allocate new VOL info struct for the pass through connector */
  new_info = (H5VL_cache_ext_info_t *)calloc(1, sizeof(H5VL_cache_ext_info_t));

  /* Increment reference count on underlying VOL ID, and copy the VOL info */
  new_info->under_vol_id = info->under_vol_id;
  /* copy Cache VOL configure file name */
  strcpy(new_info->fconfig, info->fconfig);

  H5Iinc_ref(new_info->under_vol_id);
  if (info->under_vol_info)
    H5VLcopy_connector_info(new_info->under_vol_id, &(new_info->under_vol_info),
                            info->under_vol_info);
  return new_info;
} /* end H5VL_cache_ext_info_copy() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_info_cmp
 *
 * Purpose:     Compare two of the connector's info objects, setting *cmp_value,
 *              following the same rules as strcmp().
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_info_cmp(int *cmp_value, const void *_info1,
                                      const void *_info2) {
  const H5VL_cache_ext_info_t *info1 = (const H5VL_cache_ext_info_t *)_info1;
  const H5VL_cache_ext_info_t *info2 = (const H5VL_cache_ext_info_t *)_info2;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INFO Compare\n");
#endif

  /* Sanity checks */
  assert(info1);
  assert(info2);

  /* Initialize comparison value */
  *cmp_value = 0;

  /* Compare under VOL connector classes */
  H5VLcmp_connector_cls(cmp_value, info1->under_vol_id, info2->under_vol_id);
  if (*cmp_value != 0)
    return 0;

  /* Compare under VOL connector info objects */
  H5VLcmp_connector_info(cmp_value, info1->under_vol_id, info1->under_vol_info,
                         info2->under_vol_info);
  if (*cmp_value != 0)
    return 0;

  return 0;
} /* end H5VL_cache_ext_info_cmp() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_info_free
 *
 * Purpose:     Release an info object for the connector.
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_info_free(void *_info) {
  H5VL_cache_ext_info_t *info = (H5VL_cache_ext_info_t *)_info;
  hid_t err_id;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INFO Free\n");
#endif

  err_id = H5Eget_current_stack();

  /* Release underlying VOL ID and info */
  if (info->under_vol_info)
    H5VLfree_connector_info(info->under_vol_id, info->under_vol_info);
  H5Idec_ref(info->under_vol_id);

  H5Eset_current_stack(err_id);

  /* Free pass through info object itself */
  free(info);

  return 0;
} /* end H5VL_cache_ext_info_free() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_info_to_str
 *
 * Purpose:     Serialize an info object for this connector into a string
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_info_to_str(const void *_info, char **str) {
  const H5VL_cache_ext_info_t *info = (const H5VL_cache_ext_info_t *)_info;
  H5VL_class_value_t under_value = (H5VL_class_value_t)-1;
  char *under_vol_string = NULL;
  size_t under_vol_str_len = 0;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INFO To String\n");
#endif

  /* Get value and string for underlying VOL connector */
  H5VLget_value(info->under_vol_id, &under_value);
  H5VLconnector_info_to_str(info->under_vol_info, info->under_vol_id,
                            &under_vol_string);

  /* Determine length of underlying VOL info string */
  if (under_vol_string)
    under_vol_str_len = strlen(under_vol_string);

  /* Allocate space for our info */
  *str = (char *)H5allocate_memory(32 + under_vol_str_len, (hbool_t)0);
  assert(*str);

  /* Encode our info
   * Normally we'd use snprintf() here for a little extra safety, but that
   * call had problems on Windows until recently. So, to be as
   * platform-independent as we can, we're using sprintf() instead.
   */
  sprintf(*str, "config=%s;under_vol=%u;under_info={%s}", info->fconfig,
          (unsigned)under_value, (under_vol_string ? under_vol_string : ""));
  return 0;
} /* end H5VL_cache_ext_info_to_str() */

static herr_t native_vol_info(void **_info) {
  const char *str = "under_vol=0;under_vol_info={}";
  H5VL_cache_ext_info_t *info;
  unsigned under_vol_value;
  const char *under_vol_info_start, *under_vol_info_end;
  hid_t under_vol_id;
  void *under_vol_info = NULL;

  /* Retrieve the underlying VOL connector value and info */
  sscanf(str, "under_vol=%u;", &under_vol_value);
  under_vol_id = H5VLregister_connector_by_value(
      (H5VL_class_value_t)under_vol_value, H5P_DEFAULT);

  under_vol_info_start = strchr(str, '{');
  under_vol_info_end = strrchr(str, '}');
  assert(under_vol_info_end > under_vol_info_start);
  if (under_vol_info_end != (under_vol_info_start + 1)) {
    char *under_vol_info_str;

    under_vol_info_str =
        (char *)malloc((size_t)(under_vol_info_end - under_vol_info_start));
    memcpy(under_vol_info_str, under_vol_info_start + 1,
           (size_t)((under_vol_info_end - under_vol_info_start) - 1));
    *(under_vol_info_str + (under_vol_info_end - under_vol_info_start)) = '\0';

    H5VLconnector_str_to_info(under_vol_info_str, under_vol_id,
                              &under_vol_info);

    free(under_vol_info_str);
  } /* end else */

  /* Allocate new pass-through VOL connector info and set its fields */
  info = (H5VL_cache_ext_info_t *)calloc(1, sizeof(H5VL_cache_ext_info_t));
  info->under_vol_id = under_vol_id;
  info->under_vol_info = under_vol_info;

  /* Set return value */
  *_info = info;
  return 0;
}

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_str_to_info
 *
 * Purpose:     Deserialize a string into an info object for this connector.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */

static herr_t H5VL_cache_ext_str_to_info(const char *str, void **_info) {
  H5VL_cache_ext_info_t *info;
  unsigned under_vol_value;
  const char *under_vol_info_start, *under_vol_info_end;
  hid_t under_vol_id;
  void *under_vol_info = NULL;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INFO String To Info\n");
#endif

    /* Retrieve the underlying VOL connector value and info */
#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == RANK)
    printf(" [CACHE VOL] VOL connector str: %s\n", str);
#endif

  char *lasts = NULL;
  char buf[255];
  strcpy(buf, str);

  const char *tok = strtok_r(buf, ";", &lasts);
  char fname[255];

  sscanf(tok, "config=%s", fname);

  if (!strcmp(fname, "")) {
    if (RANK == io_node()) {
      printf(
          " [CACHE VOL] **ERROR in reading configure file; make sure you have\n"
          "             'config=...;under_vol=...' in your HDF5_VOL_CONNECTOR "
          "setup\n");
    }
    exit(102);
  }
#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == RANK)
    printf(" [CACHE VOL]    config file: %s\n", fname);
#endif
  sscanf(lasts, "under_vol=%u;", &under_vol_value);
  under_vol_id = H5VLregister_connector_by_value(
      (H5VL_class_value_t)under_vol_value, H5P_DEFAULT);
  under_vol_info_start = strchr(lasts, '{');
  under_vol_info_end = strrchr(lasts, '}');
  assert(under_vol_info_end > under_vol_info_start);
  if (under_vol_info_end != (under_vol_info_start + 1)) {
    char *under_vol_info_str;

    under_vol_info_str =
        (char *)malloc((size_t)(under_vol_info_end - under_vol_info_start));
    memcpy(under_vol_info_str, under_vol_info_start + 1,
           (size_t)((under_vol_info_end - under_vol_info_start) - 1));
    *(under_vol_info_str + (under_vol_info_end - under_vol_info_start)) = '\0';

    H5VLconnector_str_to_info(under_vol_info_str, under_vol_id,
                              &under_vol_info);

    free(under_vol_info_str);
  } /* end else */

  /* Allocate new pass-through VOL connector info and set its fields */
  info = (H5VL_cache_ext_info_t *)calloc(1, sizeof(H5VL_cache_ext_info_t));
  strcpy(info->fconfig, fname);
  info->under_vol_id = under_vol_id;
  info->under_vol_info = under_vol_info;

  // create H5LS for current layer of VOL.
  H5LS_stack_t *p = H5LS_stack;
  while (p->next != NULL)
    p = p->next;
  p->H5LS = (cache_storage_t *)malloc(sizeof(cache_storage_t));
  strcpy(p->fconfig, fname);
  readLSConf(fname, p->H5LS);
#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == RANK) {
    printf(" [CACHE VOL] Cache storage setup info\n");
    printf(" [CACHE VOL] =============================\n");
    printf(" [CACHE VOL]         config file: %s\n", p->fconfig);
    printf(" [CACHE VOL]        storage path: %s\n", p->H5LS->path);
    printf(" [CACHE VOL]        storage size: %.4f GiB\n",
           p->H5LS->mspace_total / 1024. / 1024. / 1024.);
    printf(" [CACHE VOL]   write buffer size: %.4f GiB\n",
           p->H5LS->write_buffer_size / 1024. / 1024. / 1024.);
    printf(" [CACHE VOL]        storage type: %s\n", p->H5LS->type);
    printf(" [CACHE VOL]       storage scope: %s\n", p->H5LS->scope);
    printf(" [CACHE VOL]  replacement_policy: %d\n",
           (int)p->H5LS->replacement_policy);
    printf(" [CACHE VOL] =============================\n");
  }
#endif

  p->H5LS->cache_io_cls =
      (H5LS_cache_io_class_t *)malloc(sizeof(H5LS_cache_io_class_t));
  // branching out for GLOBAL and LOCAL storage
  if (!strcmp(p->H5LS->scope, "LOCAL")) {
    p->H5LS->cache_io_cls = &H5LS_cache_io_class_local_g;
    p->H5LS->mmap_cls = get_H5LS_mmap_class_t(p->H5LS->type);
  } else {
    p->H5LS->cache_io_cls = &H5LS_cache_io_class_global_g; //
  }

  p->next = (H5LS_stack_t *)malloc(sizeof(H5LS_stack_t));
  p = p->next;
  p->next = NULL;

  /* Set return value */
  *_info = info;

  return 0;
} /* end H5VL_cache_ext_str_to_info() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_get_object
 *
 * Purpose:     Retrieve the 'data' for a VOL object.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_get_object(const void *obj) {
  const H5VL_cache_ext_t *o = (const H5VL_cache_ext_t *)obj;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL Get object\n");
#endif

  return H5VLget_object(o->under_object, o->under_vol_id);
} /* end H5VL_cache_ext_get_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_get_wrap_ctx
 *
 * Purpose:     Retrieve a "wrapper context" for an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_get_wrap_ctx(const void *obj, void **wrap_ctx) {
  const H5VL_cache_ext_t *o = (const H5VL_cache_ext_t *)obj;
  H5VL_cache_ext_wrap_ctx_t *new_wrap_ctx;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL WRAP CTX Get\n");
#endif

  /* Allocate new VOL object wrapping context for the pass through connector */
  new_wrap_ctx =
      (H5VL_cache_ext_wrap_ctx_t *)calloc(1, sizeof(H5VL_cache_ext_wrap_ctx_t));

  /* Increment reference count on underlying VOL ID, and copy the VOL info */
  new_wrap_ctx->under_vol_id = o->under_vol_id;
  H5Iinc_ref(new_wrap_ctx->under_vol_id);
  H5VLget_wrap_ctx(o->under_object, o->under_vol_id,
                   &new_wrap_ctx->under_wrap_ctx);

  /* Set wrap context to return */
  *wrap_ctx = new_wrap_ctx;

  return 0;
} /* end H5VL_cache_ext_get_wrap_ctx() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_wrap_object
 *
 * Purpose:     Use a "wrapper context" to wrap a data object
 *
 * Return:      Success:    Pointer to wrapped object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_wrap_object(void *obj, H5I_type_t obj_type,
                                        void *_wrap_ctx) {
  H5VL_cache_ext_wrap_ctx_t *wrap_ctx = (H5VL_cache_ext_wrap_ctx_t *)_wrap_ctx;
  H5VL_cache_ext_t *new_obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL WRAP Object\n");
#endif

  /* Wrap the object with the underlying VOL */
  under = H5VLwrap_object(obj, obj_type, wrap_ctx->under_vol_id,
                          wrap_ctx->under_wrap_ctx);
  if (under)
    new_obj = H5VL_cache_ext_new_obj(under, wrap_ctx->under_vol_id);
  else
    new_obj = NULL;

  return new_obj;
} /* end H5VL_cache_ext_wrap_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_unwrap_object
 *
 * Purpose:     Unwrap a wrapped object, discarding the wrapper, but returning
 *		underlying object.
 *
 * Return:      Success:    Pointer to unwrapped object
 *              Failure:    NULL
 *
 *---------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_unwrap_object(void *obj) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL UNWRAP Object\n");
#endif

  /* Unrap the object with the underlying VOL */
  under = H5VLunwrap_object(o->under_object, o->under_vol_id);

  if (under)
    H5VL_cache_ext_free_obj(o);

  return under;
} /* end H5VL_cache_ext_unwrap_object() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_free_wrap_ctx
 *
 * Purpose:     Release a "wrapper context" for an object
 *
 * Note:	Take care to preserve the current HDF5 error stack
 *		when calling HDF5 API calls.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_free_wrap_ctx(void *_wrap_ctx) {
  H5VL_cache_ext_wrap_ctx_t *wrap_ctx = (H5VL_cache_ext_wrap_ctx_t *)_wrap_ctx;
  hid_t err_id;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL WRAP CTX Free\n");
#endif

  err_id = H5Eget_current_stack();

  /* Release underlying VOL ID and wrap context */
  if (wrap_ctx->under_wrap_ctx)
    H5VLfree_wrap_ctx(wrap_ctx->under_wrap_ctx, wrap_ctx->under_vol_id);
  H5Idec_ref(wrap_ctx->under_vol_id);

  H5Eset_current_stack(err_id);

  /* Free pass through wrap context object itself */
  free(wrap_ctx);

  return 0;
} /* end H5VL_cache_ext_free_wrap_ctx() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_create
 *
 * Purpose:     Creates an attribute on an object.
 *
 * Return:      Success:    Pointer to attribute object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_attr_create(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        const char *name, hid_t type_id,
                                        hid_t space_id, hid_t acpl_id,
                                        hid_t aapl_id, hid_t dxpl_id,
                                        void **req) {
  H5VL_cache_ext_t *attr;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Create\n");
#endif

  under = H5VLattr_create(o->under_object, loc_params, o->under_vol_id, name,
                          type_id, space_id, acpl_id, aapl_id, dxpl_id, req);
  if (under) {
    attr = H5VL_cache_ext_new_obj(under, o->under_vol_id);

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    attr = NULL;

  return (void *)attr;
} /* end H5VL_cache_ext_attr_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_open
 *
 * Purpose:     Opens an attribute on an object.
 *
 * Return:      Success:    Pointer to attribute object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_attr_open(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      const char *name, hid_t aapl_id,
                                      hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *attr;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Open\n");
#endif

  under = H5VLattr_open(o->under_object, loc_params, o->under_vol_id, name,
                        aapl_id, dxpl_id, req);
  if (under) {
    attr = H5VL_cache_ext_new_obj(under, o->under_vol_id);

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    attr = NULL;

  return (void *)attr;
} /* end H5VL_cache_ext_attr_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_read
 *
 * Purpose:     Reads data from attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_read(void *attr, hid_t mem_type_id, void *buf,
                                       hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)attr;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Read\n");
#endif

  ret_value = H5VLattr_read(o->under_object, o->under_vol_id, mem_type_id, buf,
                            dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_attr_read() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_write
 *
 * Purpose:     Writes data to attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_write(void *attr, hid_t mem_type_id,
                                        const void *buf, hid_t dxpl_id,
                                        void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)attr;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Write\n");
#endif

  ret_value = H5VLattr_write(o->under_object, o->under_vol_id, mem_type_id, buf,
                             dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_attr_write() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_get
 *
 * Purpose:     Gets information about an attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_get(void *obj, H5VL_attr_get_args_t *args,
                                      hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Get\n");
#endif

  ret_value =
      H5VLattr_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_attr_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_specific
 *
 * Purpose:     Specific operation on attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_specific(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_attr_specific_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Specific\n");
#endif

  ret_value = H5VLattr_specific(o->under_object, loc_params, o->under_vol_id,
                                args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_attr_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_optional
 *
 * Purpose:     Perform a connector-specific operation on an attribute
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_optional(void *obj,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Optional\n");
#endif

  ret_value =
      H5VLattr_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_attr_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_attr_close
 *
 * Purpose:     Closes an attribute.
 *
 * Return:      Success:    0
 *              Failure:    -1, attr not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_attr_close(void *attr, hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)attr;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL ATTRIBUTE Close\n");
#endif

  ret_value = H5VLattr_close(o->under_object, o->under_vol_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  /* Release our wrapper, if underlying attribute was closed */
  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);

  return ret_value;
} /* end H5VL_cache_ext_attr_close() */

static hid_t dataset_get_type(void *dset, hid_t driver_id, hid_t dxpl_id,
                              void **req) {
  H5VL_dataset_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_DATASET_GET_TYPE;
  vol_cb_args.args.get_type.type_id = H5I_INVALID_HID;

  if (H5VLdataset_get(dset, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return H5I_INVALID_HID;

  return vol_cb_args.args.get_type.type_id;
}

static hid_t dataset_get_space(void *dset, hid_t driver_id, hid_t dxpl_id,
                               void **req) {
  H5VL_dataset_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_DATASET_GET_SPACE;
  vol_cb_args.args.get_space.space_id = H5I_INVALID_HID;

  if (H5VLdataset_get(dset, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return H5I_INVALID_HID;

  return vol_cb_args.args.get_space.space_id;
}

static hid_t dataset_get_dcpl(void *dset, hid_t driver_id, hid_t dxpl_id,
                              void **req) {
  H5VL_dataset_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_DATASET_GET_DCPL;
  vol_cb_args.args.get_dcpl.dcpl_id = H5I_INVALID_HID;

  if (H5VLdataset_get(dset, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return H5I_INVALID_HID;

  return vol_cb_args.args.get_dcpl.dcpl_id;
}

static hid_t dataset_get_dapl(void *dset, hid_t driver_id, hid_t dxpl_id,
                              void **req) {
  H5VL_dataset_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_DATASET_GET_DAPL;
  vol_cb_args.args.get_dapl.dapl_id = H5I_INVALID_HID;

  if (H5VLdataset_get(dset, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return H5I_INVALID_HID;

  return vol_cb_args.args.get_dapl.dapl_id;
}

static hid_t group_get_gapl(void *group, hid_t driver_id, hid_t dxpl_id,
                            void **req) {
  H5VL_dataset_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  // vol_cb_args.op_type = H5VL_GROUP_GET_GAPL;
  // vol_cb_args.args.get_dapl.dapl_id = H5I_INVALID_HID;

  // if (H5VLgroup_get(group, driver_id, &vol_cb_args, dxpl_id, req) < 0)
  // return H5I_INVALID_HID;
  return H5P_DEFAULT;
  // return vol_cb_args.args.get_dapl.dapl_id;
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_create
 *
 * Purpose:     Creates a dataset in a container
 *
 * Return:      Success:    Pointer to a dataset object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_dataset_create(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           const char *name, hid_t lcpl_id,
                                           hid_t type_id, hid_t space_id,
                                           hid_t dcpl_id, hid_t dapl_id,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *dset;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Create\n");
#endif

  under = H5VLdataset_create(o->under_object, loc_params, o->under_vol_id, name,
                             lcpl_id, type_id, space_id, dcpl_id, dapl_id,
                             dxpl_id, req);
  if (under) {
    dset = H5VL_cache_ext_new_obj(under, o->under_vol_id);
    /* inherit cache information from loc */
    dset->parent = obj;
    dset->H5DWMM = o->H5DWMM;
    dset->write_cache = o->write_cache;
    dset->read_cache = o->read_cache;
    dset->num_request_dataset = 0;
    dset->H5DRMM = NULL;
    dset->H5LS = o->H5LS;
    dset->async_pause = o->async_pause;
    if (o->write_cache || o->read_cache) {
      dset->H5LS->previous_write_req = NULL;
      dset->es_id = H5EScreate();
      dset_args_t *args = (dset_args_t *)malloc(sizeof(dset_args_t));
      args->name = name;
      args->loc_params = loc_params;
      args->lcpl_id = lcpl_id;
      args->type_id = type_id;
      args->space_id = space_id;
      args->dcpl_id = dcpl_id;
      args->dapl_id = dapl_id;
      args->dxpl_id = dxpl_id;
      dset->H5LS->cache_io_cls->create_dataset_cache((void *)dset, (void *)args,
                                                     req);
      free(args);
    }

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    dset = NULL;

  return (void *)dset;
} /* end H5VL_cache_ext_dataset_create() */
hsize_t round_page(hsize_t s) {
  if (s % PAGESIZE == 0 || s < PAGESIZE)
    return s;
  return (s / PAGESIZE + 1) * PAGESIZE;
}
static herr_t H5VL_cache_ext_dataset_mmap_remap(void *obj) {
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  // created a memory mapped file on the local storage. And create a MPI_win
  hsize_t ss = round_page(dset->H5DRMM->dset.size);
  if (strcmp(dset->H5LS->type, "MEMORY") != 0) {
    // msync(dset->H5DRMM->mmap->buf, ss, MS_SYNC);
    double t0 = MPI_Wtime();
    munmap(dset->H5DRMM->mmap->buf, ss);
#ifdef __linux__
#ifndef NDEBUG
    if (dset->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] hint drop caches\n");
#endif
    posix_fadvise(dset->H5DRMM->mmap->fd, 0, ss, POSIX_FADV_DONTNEED);
#endif
    fsync(dset->H5DRMM->mmap->fd);
    close(dset->H5DRMM->mmap->fd);
    MPI_Win_free(&dset->H5DRMM->mpi->win);
    double t1 = MPI_Wtime();
#ifndef NDEBUG
    if (dset->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] close the files, remove the caches\n");
#endif
    char tmp[252];
    strcpy(tmp, dset->H5DRMM->mmap->fname);
    strcat(dset->H5DRMM->mmap->fname, "p");
    char cmd[300];
    strcpy(cmd, "cp ");
    strcat(cmd, tmp);
    strcat(cmd, " ");
    strcat(cmd, dset->H5DRMM->mmap->fname);
    //    sprintf(cmd, "cp %s %s", tmp, dset->H5DRMM->mmap->fname);
    system(cmd);
    remove(tmp);
    dset->H5DRMM->mmap->fd = open(dset->H5DRMM->mmap->fname, O_RDWR);
    dset->H5DRMM->mmap->buf =
        mmap(NULL, ss, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE,
             dset->H5DRMM->mmap->fd, 0);
    MPI_Win_create(dset->H5DRMM->mmap->buf, ss, dset->H5DRMM->dset.esize,
                   MPI_INFO_NULL, dset->H5DRMM->mpi->comm,
                   &dset->H5DRMM->mpi->win);
    double t2 = MPI_Wtime();
#ifndef NDEBUG
    if (dset->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] Remap time: %f(rm) %f(re)\n", t1 - t0, t2 - t1);
    LOG(dset->H5DRMM->mpi->rank, "Remap MMAP");
#endif
  }
  return SUCCEED;
}

#define BLOCK_LIMIT 1073741824

#if H5_VERSION_GE(1, 13, 3)
static herr_t H5VL_cache_ext_dataset_prefetch_async(void *obj, hid_t fspace,
                                                    hid_t plist_id,
                                                    void *req_list) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Prefetch async\n");
#endif
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  herr_t ret_value = SUCCEED;
  if (dset->read_cache) {
    int ndims = H5Sget_simple_extent_ndims(fspace);
    int *samples = (int *)malloc(sizeof(int) * dset->H5DRMM->dset.ns_loc);
    int nblock = 1;
    int nsample_per_block = dset->H5DRMM->dset.ns_loc;
    if (dset->H5DRMM->dset.size > BLOCK_LIMIT) {
      nblock = dset->H5DRMM->dset.size / BLOCK_LIMIT;
      nsample_per_block = dset->H5DRMM->dset.ns_loc / nblock;
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **Split into %d (+1) block(s) to write the data "
               "to cache storage\n",
               nblock);
#endif
    }
    int i;
    for (i = 0; i < dset->H5DRMM->dset.ns_loc; i++)
      samples[i] = dset->H5DRMM->dset.s_offset + i;
#ifndef NDEBUG
    if (debug_level() > 2)
      printf(" [CACHE VOL] Number of samples per proc: %ld; offset: %ld\n",
             dset->H5DRMM->dset.ns_loc, dset->H5DRMM->dset.s_offset);
#endif
    char *p = (char *)dset->H5DRMM->mmap->buf;
    request_list_t *r = req_list;
    int n;
    for (n = 0; n < nblock; n++) {
      r = (request_list_t *)malloc(sizeof(request_list_t));
      r->req = NULL;
      r->next = NULL;
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[n * nsample_per_block],
                                 nsample_per_block, &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset = dset->H5DRMM->dset.sample.size * n * nsample_per_block;
      // temporally fix
      void *ptr = &p[offset];
      ret_value = H5VLdataset_read(1, &dset->under_object, dset->under_vol_id,
                                   &dset->H5DRMM->dset.h5_datatype, &mspace,
                                   &fs_cpy, plist_id, &ptr, &r->req);
      r = r->next;
    }
    if (dset->H5DRMM->dset.ns_loc % nsample_per_block != 0) {
      hid_t fs_cpy = H5Scopy(fspace);
      r = (request_list_t *)malloc(sizeof(request_list_t));
      r->req = NULL;
      r->next = NULL;
      set_hyperslab_from_samples(&samples[nblock * nsample_per_block],
                                 dset->H5DRMM->dset.ns_loc % nsample_per_block,
                                 &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = dset->H5DRMM->dset.ns_loc % nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset =
          dset->H5DRMM->dset.sample.size * nblock * nsample_per_block;
      // We only assume prefetching on dataset, not multiple.
      void *ptr = &p[offset];
      ret_value = H5VLdataset_read(1, &dset->under_object, dset->under_vol_id,
                                   &dset->H5DRMM->dset.h5_datatype, &mspace,
                                   &fs_cpy, plist_id, &p, &r->req);
      nblock = nblock + 1;
    }
  }
  return ret_value;
}
#else
static herr_t H5VL_cache_ext_dataset_prefetch_async(void *obj, hid_t fspace,
                                                    hid_t plist_id,
                                                    void *req_list) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Prefetch async\n");
#endif
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  herr_t ret_value = SUCCEED;
  if (dset->read_cache) {
    int ndims = H5Sget_simple_extent_ndims(fspace);
    int *samples = (int *)malloc(sizeof(int) * dset->H5DRMM->dset.ns_loc);
    int nblock = 1;
    int nsample_per_block = dset->H5DRMM->dset.ns_loc;
    if (dset->H5DRMM->dset.size > BLOCK_LIMIT) {
      nblock = dset->H5DRMM->dset.size / BLOCK_LIMIT;
      nsample_per_block = dset->H5DRMM->dset.ns_loc / nblock;
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **Split into %d (+1) block(s) to write the data "
               "to cache storage\n",
               nblock);
#endif
    }
    int i;
    for (i = 0; i < dset->H5DRMM->dset.ns_loc; i++)
      samples[i] = dset->H5DRMM->dset.s_offset + i;
#ifndef NDEBUG
    if (debug_level() > 2)
      printf(" [CACHE VOL] Number of samples per proc: %ld; offset: %ld\n",
             dset->H5DRMM->dset.ns_loc, dset->H5DRMM->dset.s_offset);
#endif
    char *p = (char *)dset->H5DRMM->mmap->buf;
    request_list_t *r = req_list;
    int n;
    for (n = 0; n < nblock; n++) {
      r = (request_list_t *)malloc(sizeof(request_list_t));
      r->req = NULL;
      r->next = NULL;
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[n * nsample_per_block],
                                 nsample_per_block, &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset = dset->H5DRMM->dset.sample.size * n * nsample_per_block;
      ret_value = H5VLdataset_read(dset->under_object, dset->under_vol_id,
                                   dset->H5DRMM->dset.h5_datatype, mspace,
                                   fs_cpy, plist_id, &p[offset], &r->req);
      r = r->next;
    }
    if (dset->H5DRMM->dset.ns_loc % nsample_per_block != 0) {
      hid_t fs_cpy = H5Scopy(fspace);
      r = (request_list_t *)malloc(sizeof(request_list_t));
      r->req = NULL;
      r->next = NULL;
      set_hyperslab_from_samples(&samples[nblock * nsample_per_block],
                                 dset->H5DRMM->dset.ns_loc % nsample_per_block,
                                 &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = dset->H5DRMM->dset.ns_loc % nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset =
          dset->H5DRMM->dset.sample.size * nblock * nsample_per_block;
      ret_value = H5VLdataset_read(dset->under_object, dset->under_vol_id,
                                   dset->H5DRMM->dset.h5_datatype, mspace,
                                   fs_cpy, plist_id, &p[offset], &r->req);
      nblock = nblock + 1;
    }
  }
  return ret_value;
}
#endif

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_open
 *
 * Purpose:     Opens a dataset in a container
 *
 * Return:      Success:    Pointer to a dataset object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_dataset_open(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         const char *name, hid_t dapl_id,
                                         hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *dset;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Open\n");
#endif

  under = H5VLdataset_open(o->under_object, loc_params, o->under_vol_id, name,
                           dapl_id, dxpl_id, req);
  if (under) {
    dset = H5VL_cache_ext_new_obj(under, o->under_vol_id);
    /* Inherit Cache information from obj */
    dset->parent = obj;
    dset->H5DWMM = o->H5DWMM;
    dset->write_cache = o->write_cache;
    dset->read_cache = o->read_cache;
    dset->num_request_dataset = 0;
    dset->H5DRMM = NULL;
    dset->H5LS = o->H5LS;
    dset->async_pause = o->async_pause;
    /* setup read cache */
    if (dset->read_cache || dset->write_cache) {
      dset->es_id = H5EScreate();
      dset_args_t *args = (dset_args_t *)malloc(sizeof(dset_args_t));
      args->type_id = dataset_get_type(dset->under_object, dset->under_vol_id,
                                       dxpl_id, NULL);
      args->space_id = dataset_get_space(dset->under_object, dset->under_vol_id,
                                         dxpl_id, NULL);
      args->dcpl_id = dataset_get_dcpl(dset->under_object, dset->under_vol_id,
                                       dxpl_id, NULL);
      args->lcpl_id = H5Pcreate(H5P_LINK_CREATE);
      args->name = name;
      args->loc_params = loc_params;
      args->dapl_id = dapl_id;
      args->dxpl_id = dxpl_id;
      dset->H5LS->cache_io_cls->create_dataset_cache((void *)dset, (void *)args,
                                                     req);
      free(args);
      if (getenv("DATASET_PREFETCH_AT_OPEN")) {
        if (dset->read_cache &&
            !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes")) {
#ifndef NDEBUG
          if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
            printf(" [CACHE VOL] DATASET_PREFETCH_AT_OPEN = yes\n");
#endif
          H5VL_cache_ext_dataset_prefetch_async(dset, args->space_id, dxpl_id,
                                                dset->prefetch_req);
        }
      }
    }
    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  } /* end if */
  else
    dset = NULL;

  return (void *)dset;
} /* end H5VL_cache_ext_dataset_open() */

herr_t H5VL_cache_ext_dataset_prefetch_wait(void *dset) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  H5VL_request_status_t status;
  request_list_t *r = o->prefetch_req;
  while (r != NULL) {
    H5VLrequest_wait(r->req, o->under_vol_id, INF, &status);
    r = r->next;
  }
  hsize_t ss = round_page(o->H5DRMM->dset.size);
  if (o->H5LS->path != NULL)
    msync(o->H5DRMM->mmap->buf, ss, MS_SYNC);
  o->H5DRMM->io->dset_cached = true;
  o->H5DRMM->io->batch_cached = true;
  return 0;
}
/*
   Dataset prefetch function: currently, we prefetch the entire dataset into the
   storage.
*/
#if H5_VERSION_GE(1, 13, 3)

static herr_t H5VL_cache_ext_dataset_prefetch(void *obj, hid_t fspace,
                                              hid_t plist_id, void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Prefetch\n");
#endif
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;
  if (dset->read_cache) {
    if (getenv("DATASET_PREFETCH_AT_OPEN")) {
      if (dset->read_cache &&
          !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes")) {
#ifndef NDEBUG
        if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
          printf(" [CACHE VOL] prefetched async called\n");
#endif
        return SUCCEED;
      }
    }
    int ndims = H5Sget_simple_extent_ndims(fspace);
    int *samples = (int *)malloc(sizeof(int) * dset->H5DRMM->dset.ns_loc);
    int nblock = 1;
    int nsample_per_block = dset->H5DRMM->dset.ns_loc;
    if (dset->H5DRMM->dset.size > BLOCK_LIMIT) {
      nblock = dset->H5DRMM->dset.size / BLOCK_LIMIT;
      nsample_per_block = dset->H5DRMM->dset.ns_loc / nblock;
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **Split into %d (+1) block(s) to write data to "
               "the cache storage\n",
               nblock);
#endif
    }
    int i;
    for (i = 0; i < dset->H5DRMM->dset.ns_loc; i++)
      samples[i] = dset->H5DRMM->dset.s_offset + i;
#ifndef NDEBUG
    if (debug_level() > 2)
      printf(" [CACHE VOL] Number of samples: %ld; offset: %ld\n",
             dset->H5DRMM->dset.ns_loc, dset->H5DRMM->dset.s_offset);
#endif
    char *p = (char *)dset->H5DRMM->mmap->buf;
    int n;
    for (n = 0; n < nblock; n++) {
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[n * nsample_per_block],
                                 nsample_per_block, &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset = dset->H5DRMM->dset.sample.size * n * nsample_per_block;
#if H5_VERSION_GE(1, 13, 3)
      void *ptr = &p[offset];
      ret_value = H5VLdataset_read(1, dset->under_object, dset->under_vol_id,
                                   &dset->H5DRMM->dset.h5_datatype, &mspace,
                                   &fs_cpy, plist_id, &ptr, NULL);
#else
      ret_value = H5VLdataset_read(dset->under_object, dset->under_vol_id,
                                   dset->H5DRMM->dset.h5_datatype, mspace,
                                   fs_cpy, plist_id, &p[offset], NULL);
#endif
    }
    if (dset->H5DRMM->dset.ns_loc % nsample_per_block != 0) {
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[nblock * nsample_per_block],
                                 dset->H5DRMM->dset.ns_loc % nsample_per_block,
                                 &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = dset->H5DRMM->dset.ns_loc % nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset =
          dset->H5DRMM->dset.sample.size * nblock * nsample_per_block;
      void *ptr = &p[offset];
      ret_value = H5VLdataset_read(1, &dset->under_object, dset->under_vol_id,
                                   &dset->H5DRMM->dset.h5_datatype, &mspace,
                                   &fs_cpy, plist_id, &ptr, NULL);
      nblock = nblock + 1;
    }
    if (ret_value == 0) {
      hsize_t ss = round_page(dset->H5DRMM->dset.size);
      if (dset->H5LS->path != NULL)
        msync(dset->H5DRMM->mmap->buf, dset->H5DRMM->dset.size, MS_SYNC);
      dset->H5DRMM->io->dset_cached = true;
      dset->H5DRMM->io->batch_cached = true;
    }
    return ret_value;
  } else {
#ifndef NDEBUG
    printf(" [CACHE VOL] HDF5_CACHE_RD is not set, doing nothing here for "
           "dataset_prefetch\n");
#endif
    return 0;
  }
}

#else
static herr_t H5VL_cache_ext_dataset_prefetch(void *obj, hid_t fspace,
                                              hid_t plist_id, void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Prefetch\n");
#endif
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;
  if (dset->read_cache) {
    if (getenv("DATASET_PREFETCH_AT_OPEN")) {
      if (dset->read_cache &&
          !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes")) {
#ifndef NDEBUG
        if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
          printf(" [CACHE VOL] prefetched async called\n");
#endif
        return SUCCEED;
      }
    }
    int ndims = H5Sget_simple_extent_ndims(fspace);
    int *samples = (int *)malloc(sizeof(int) * dset->H5DRMM->dset.ns_loc);
    int nblock = 1;
    int nsample_per_block = dset->H5DRMM->dset.ns_loc;
    if (dset->H5DRMM->dset.size > BLOCK_LIMIT) {
      nblock = dset->H5DRMM->dset.size / BLOCK_LIMIT;
      nsample_per_block = dset->H5DRMM->dset.ns_loc / nblock;
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **Split into %d (+1) block(s) to write data to "
               "the cache storage\n",
               nblock);
#endif
    }
    int i;
    for (i = 0; i < dset->H5DRMM->dset.ns_loc; i++)
      samples[i] = dset->H5DRMM->dset.s_offset + i;
#ifndef NDEBUG
    if (debug_level() > 2)
      printf(" [CACHE VOL] Number of samples: %ld; offset: %ld\n",
             dset->H5DRMM->dset.ns_loc, dset->H5DRMM->dset.s_offset);
#endif
    char *p = (char *)dset->H5DRMM->mmap->buf;
    int n;
    for (n = 0; n < nblock; n++) {
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[n * nsample_per_block],
                                 nsample_per_block, &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset = dset->H5DRMM->dset.sample.size * n * nsample_per_block;
      ret_value = H5VLdataset_read(dset->under_object, dset->under_vol_id,
                                   dset->H5DRMM->dset.h5_datatype, mspace,
                                   fs_cpy, plist_id, &p[offset], NULL);
    }
    if (dset->H5DRMM->dset.ns_loc % nsample_per_block != 0) {
      hid_t fs_cpy = H5Scopy(fspace);
      set_hyperslab_from_samples(&samples[nblock * nsample_per_block],
                                 dset->H5DRMM->dset.ns_loc % nsample_per_block,
                                 &fs_cpy);
      hsize_t *ldims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
      H5Sget_simple_extent_dims(fs_cpy, ldims, NULL);
      ldims[0] = dset->H5DRMM->dset.ns_loc % nsample_per_block;
      hid_t mspace = H5Screate_simple(ndims, ldims, NULL);
      hsize_t offset =
          dset->H5DRMM->dset.sample.size * nblock * nsample_per_block;
      ret_value = H5VLdataset_read(dset->under_object, dset->under_vol_id,
                                   dset->H5DRMM->dset.h5_datatype, mspace,
                                   fs_cpy, plist_id, &p[offset], NULL);
      nblock = nblock + 1;
    }
    if (ret_value == 0) {
      hsize_t ss = round_page(dset->H5DRMM->dset.size);
      if (dset->H5LS->path != NULL)
        msync(dset->H5DRMM->mmap->buf, dset->H5DRMM->dset.size, MS_SYNC);
      dset->H5DRMM->io->dset_cached = true;
      dset->H5DRMM->io->batch_cached = true;
    }
    return ret_value;
  } else {
#ifndef NDEBUG
    printf(" [CACHE VOL] HDF5_CACHE_RD is not set, doing nothing here for "
           "dataset_prefetch\n");
#endif
    return 0;
  }
}
#endif

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_read_to_cache
 *
 * Purpose:     Reads data elements from a dataset into a buffer and stores
 *              a copy to the local storage
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */

#if H5_VERSION_GE(1, 13, 3)
static herr_t H5VL_cache_ext_dataset_read_to_cache(
    size_t count, void *dset[], hid_t mem_type_id[], hid_t mem_space_id[],
    hid_t file_space_id[], hid_t plist_id, void *buf[], void **req) {
  void *obj_local;
  void **obj = &obj_local;
  size_t i;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read to cache\n");
#endif
  if (count > 1)
    if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
      return -1;
  // get the under vol object from all the dataset
  for (i = 0; i < count; i++) {
    obj[i] = ((H5VL_cache_ext_t *)dset[i])->under_object;
    if (((H5VL_cache_ext_t *)dset[i])->under_vol_id !=
        ((H5VL_cache_ext_t *)dset[0])->under_vol_id)
      return -1;
  }

  // calling the under H5VLdataset_read
  ret_value = H5VLdataset_read(
      count, obj, ((H5VL_cache_ext_t *)dset[0])->under_vol_id, mem_type_id,
      mem_space_id, file_space_id, plist_id, buf, NULL);
  /* Saving the read buffer(s) to local storage */
  if (((H5VL_cache_ext_t *)dset[0])->read_cache)
    for (i = 0; i < count; i++)
      ((H5VL_cache_ext_t *)dset[i])
          ->H5LS->cache_io_cls->write_data_to_cache2(
              dset[i], mem_type_id[i], mem_space_id[i], file_space_id[i],
              plist_id, buf[i], req);
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req,
                                  ((H5VL_cache_ext_t *)dset[0])->under_vol_id);
  return ret_value;
} /* end H5VL_cache_ext_dataset_read_to_cache() */

#else
static herr_t
H5VL_cache_ext_dataset_read_to_cache(void *dset, hid_t mem_type_id,
                                     hid_t mem_space_id, hid_t file_space_id,
                                     hid_t plist_id, void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read to cache\n");
#endif
  /// need to make this call blocking
  ret_value =
      H5VLdataset_read(o->under_object, o->under_vol_id, mem_type_id,
                       mem_space_id, file_space_id, plist_id, buf, NULL);
  /* Saving the read buffer to local storage */
  if (o->read_cache)
    o->H5LS->cache_io_cls->write_data_to_cache2(
        dset, mem_type_id, mem_space_id, file_space_id, plist_id, buf, req);
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  return ret_value;
} /* end H5VL_cache_ext_dataset_read_to_cache() */
#endif
/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_wait
 *
 * Purpose:     Wait (with a timeout) for an async operation to complete
 *
 * Note:        Releases the request if the operation has completed and the
 *              connector callback succeeds
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_request_wait(void *obj, uint64_t timeout,
                                          H5VL_request_status_t *status) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Wait\n");
#endif

  ret_value =
      H5VLrequest_wait(o->under_object, o->under_vol_id, timeout, status);

  return ret_value;
} /* end H5VL_cache_ext_request_wait() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_read
 *
 * Purpose:     Reads data elements from a dataset into a buffer.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
#if H5_VERSION_GE(1, 13, 3)
static herr_t H5VL_cache_ext_dataset_read(size_t count, void *dset[],
                                          hid_t mem_type_id[],
                                          hid_t mem_space_id[],
                                          hid_t file_space_id[], hid_t plist_id,
                                          void *buf[], void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read\n");
#endif
  void *obj_local;
  void **obj = &obj_local;
  herr_t ret_value;
  size_t i;
  /* Allocate obj array if necessary */
  if (count > 1)
    if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
      return -1;

  /* Build obj array */
  for (i = 0; i < count; i++) {
    /* Get the object */
    obj[i] = ((H5VL_cache_ext_t *)dset[i])->under_object;

    /* Make sure the class matches */
    if (((H5VL_cache_ext_t *)dset[i])->under_vol_id !=
        ((H5VL_cache_ext_t *)dset[0])->under_vol_id)
      return -1;
  }

  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset[0];

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read\n");
#endif
  if (o->read_cache) {
    if (getenv("DATASET_PREFETCH_AT_OPEN") && o->read_cache &&
        !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes") &&
        !o->H5DRMM->io->dset_cached)
      H5VL_cache_ext_dataset_prefetch_wait(dset);
#ifndef NDEBUG
    if (debug_level() > 0)
      printf(" [CACHE VOL] [%d]: %d samples (cached); %zu samples (total); %d "
             "(dataset cached?)\n",
             o->H5DRMM->mpi->rank, o->H5DRMM->dset.ns_cached,
             o->H5DRMM->dset.ns_loc, o->H5DRMM->io->dset_cached);
#endif
    if (!o->H5DRMM->io->dset_cached) {
      ret_value =
          H5VLdataset_read(count, obj, o->under_vol_id, mem_type_id,
                           mem_space_id, file_space_id, plist_id, buf, req);
      for (size_t i = 0; i < count; i++)
        o->H5LS->cache_io_cls->write_data_to_cache2(
            dset[i], mem_type_id[i], mem_space_id[i], file_space_id[i],
            plist_id, buf[i], req);
    } else {
      for (size_t i = 0; i < count; i++)
        ret_value = o->H5LS->cache_io_cls->read_data_from_cache(
            dset[i], mem_type_id[i], mem_space_id[i], file_space_id[i],
            plist_id, buf[i], req);
    }
  } else {
    ret_value =
        H5VLdataset_read(count, obj, o->under_vol_id, mem_type_id, mem_space_id,
                         file_space_id, plist_id, buf, req);
  }
  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req,
                                  ((H5VL_cache_ext_t *)dset[0])->under_vol_id);
  if (obj != &obj_local)
    free(obj);
  return ret_value;
} /* end H5VL_cache_ext_dataset_read() */
#else
static herr_t H5VL_cache_ext_dataset_read(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read\n");
#endif
  if (o->read_cache) {
    if (getenv("DATASET_PREFETCH_AT_OPEN") && o->read_cache &&
        !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes") &&
        !o->H5DRMM->io->dset_cached)
      H5VL_cache_ext_dataset_prefetch_wait(dset);
#ifndef NDEBUG
    if (debug_level() > 0)
      printf(" [CACHE VOL] [%d]: %d samples (cached); %zu samples (total); %d "
             "(dataset cached?)\n",
             o->H5DRMM->mpi->rank, o->H5DRMM->dset.ns_cached,
             o->H5DRMM->dset.ns_loc, o->H5DRMM->io->dset_cached);
#endif
    if (!o->H5DRMM->io->dset_cached) {
      ret_value =
          H5VLdataset_read(o->under_object, o->under_vol_id, mem_type_id,
                           mem_space_id, file_space_id, plist_id, buf, req);
      o->H5LS->cache_io_cls->write_data_to_cache2(
          dset, mem_type_id, mem_space_id, file_space_id, plist_id, buf, req);
    } else
      ret_value = o->H5LS->cache_io_cls->read_data_from_cache(
          dset, mem_type_id, mem_space_id, file_space_id, plist_id, buf, req);
  } else {
    ret_value =
        H5VLdataset_read(o->under_object, o->under_vol_id, mem_type_id,
                         mem_space_id, file_space_id, plist_id, buf, req);
  }

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_dataset_read() */
#endif
/* Waiting for the dataset write task to finish to free up cache space */
static herr_t free_cache_space_from_dataset(void *dset, hsize_t size) {

  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
#ifndef NDEBUG
  if (debug_level() > 1 && o->H5DWMM->mpi->rank == io_node())
    printf(" [CACHE VOL] free cache space from dataset\n");
#endif
  if (!strcmp(o->H5LS->scope, "GLOBAL"))
    return SUCCEED;
  H5VL_class_value_t under_value;

  H5VLget_value(o->under_vol_id, &under_value);
  if (under_value != H5VL_ASYNC_VALUE) {
    if (RANK == io_node())
      printf(" [CACHE VOL] **WARNING: Do not have Async VOL underneath it.\n");
    return FAIL;
  }
  if (o->H5DWMM->cache->mspace_per_rank_total < size) {
    if (io_node() == o->H5DWMM->mpi->rank)
      printf(" [CACHE VOL] **WARNING: size of the dataset to be writen exceeds "
             "the size of "
             "the node-local storage specified; \n"
             "             try to increase HDF5_CACHE_WRITE_BUFFER_SIZE to at "
             "least %d\n",
             size * o->H5DWMM->mpi->ppn);
    return FAIL;
  }
  if (o->H5DWMM->cache->mspace_per_rank_left > size) {
    return SUCCEED;
  }
  H5VL_request_status_t status;

  if (o->H5DWMM->mmap->offset + size >
      o->H5DWMM->cache->mspace_per_rank_total) {
    double available = o->H5DWMM->cache->mspace_per_rank_left;
    while ((o->H5DWMM->io->current_request != NULL) && (size > available)) {
#ifndef NDEBUG
      if (debug_level() > 2 && io_node() == o->H5DWMM->mpi->rank)
        printf(" [CACHE VOL] request wait(jobid: %d), current available space: "
               "%.5f GiB \n",
               o->H5DWMM->io->current_request->id,
               available / 1024. / 1024. / 1024);
#endif
      H5async_start(o->H5DWMM->io->current_request->req);
      H5VLrequest_wait(o->H5DWMM->io->current_request->req, o->under_vol_id,
                       INF, &status);
#ifndef NDEBUG
      if (debug_level() > 2 && io_node() == o->H5DWMM->mpi->rank)
        printf(" [CACHE VOL] **Task %d finished\n",
               o->H5DWMM->io->current_request->id);
#endif
      o->H5DWMM->io->num_request--;
#if H5_VERSION_GE(1, 13, 3)
      for (size_t i = 0; i < o->H5DWMM->io->current_request->count; i++) {
        H5VL_cache_ext_t *d =
            (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj[i];
        d->num_request_dataset--;
      }
#else
      H5VL_cache_ext_t *d =
          (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj;
      d->num_request_dataset--;
#endif
      available = available + round_page(o->H5DWMM->io->current_request->size);
#ifndef NDEBUG
      if (debug_level() > 2 && io_node() == o->H5DWMM->mpi->rank)
        printf(" [CACHE VOL] request wait(jobid: %d), current available space: "
               "%.5f GiB \n",
               o->H5DWMM->io->current_request->id,
               available / 1024. / 1024. / 1024.);
#endif
      o->H5DWMM->io->current_request = o->H5DWMM->io->current_request->next;
    }
    o->H5DWMM->cache->mspace_per_rank_left = available;
    o->H5DWMM->mmap->offset = 0;
#ifndef NDEBUG
    if (debug_level() > 2 && io_node() == o->H5DWMM->mpi->rank)
      printf(" [CACHE VOL] request wait(jobid: %d), current available space: "
             "%.5f \n",
             o->H5DWMM->io->current_request->id,
             available / 1024. / 1024. / 1024.);
#endif
  } else if (o->H5DWMM->mmap->offset < o->H5DWMM->io->current_request->offset) {
    double available = o->H5DWMM->cache->mspace_per_rank_left;
    while ((o->H5DWMM->io->current_request != NULL) &&
           (o->H5DWMM->io->current_request->offset <
            o->H5DWMM->mmap->offset + size)) {
      H5async_start(o->H5DWMM->io->current_request->req);
      H5VLrequest_wait(o->H5DWMM->io->current_request->req, o->under_vol_id,
                       INF, &status);
      o->H5DWMM->io->num_request--;
#if H5_VERSION_GE(1, 13, 3)
      for (size_t i = 0; i < o->H5DWMM->io->current_request->count; i++) {
        H5VL_cache_ext_t *d =
            (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj[i];
        d->num_request_dataset--;
      }
#else
      H5VL_cache_ext_t *d =
          (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj;
      d->num_request_dataset--;
#endif
      available = available + round_page(o->H5DWMM->io->current_request->size);
      o->H5DWMM->io->current_request = o->H5DWMM->io->current_request->next;
    }
    o->H5DWMM->cache->mspace_per_rank_left = (hsize_t)(available);
  }
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] free_space_from_dataset (after): "
           "o->H5DWMM->cache->mspace_per_rank_left, size: %.5f GiB, %.5f GiB\n",
           o->H5DWMM->cache->mspace_per_rank_left / 1024. / 1024. / 1024.,
           size / 1024. / 1024. / 1024.);
#endif
  if (o->H5DWMM->cache->mspace_per_rank_left >= size)
    return SUCCEED;
  else
    return FAIL;
}

/*
  This is to add current task to the request-list, and return a reference to the
  current request.
 */
#if H5_VERSION_GE(1, 13, 3)
static herr_t
add_current_write_task_to_queue(size_t count, void *dset[], hid_t mem_type_id[],
                                hid_t mem_space_id[], hid_t file_space_id[],
                                hid_t plist_id, const void *buf[]) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset[0];
#ifndef NDEBUG
  if (debug_level() > 1 && o->H5DWMM->mpi->rank == io_node())
    printf(" [CACHE VOL] Adding current write task %d to queue\n",
           o->H5DWMM->io->request_list->id);
#endif
  // writing data to the cache storage
  size_t i;
  o->H5DWMM->io->request_list->buf = (void **)calloc(count, sizeof(void *));

  for (i = 0; i < count; i++)
    o->H5DWMM->io->request_list->buf[i] =
        o->H5LS->cache_io_cls->write_data_to_cache(
            dset[i], mem_type_id[i], mem_space_id[i], file_space_id[i],
            plist_id, buf[i], NULL);

  hsize_t size = 0;
  for (i = 0; i < count; i++) {
    size += get_buf_size(mem_space_id[i], mem_type_id[i]);
  }
  // building request list

  o->H5DWMM->io->request_list->offset = o->H5DWMM->mmap->offset;
  o->H5DWMM->mmap->offset += round_page(size);
  o->H5DWMM->cache->mspace_per_rank_left =
      o->H5DWMM->cache->mspace_per_rank_left - round_page(size);
#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank)
    printf(
        " [CACHE VOL] offset, space left (per rank), total storage (per rank) "
        "%llu, %llu, %llu\n",
        o->H5DWMM->mmap->offset, o->H5DWMM->cache->mspace_per_rank_left,
        o->H5DWMM->cache->mspace_per_rank_total);
#endif
  o->H5DWMM->io->request_list->count = count;
  task_data_t *r = (task_data_t *)o->H5DWMM->io->request_list;
  r->dataset_obj = (void **)calloc(count, sizeof(void *));
  r->mem_type_id = (hid_t *)calloc(count, sizeof(hid_t));
  r->mem_space_id = (hid_t *)calloc(count, sizeof(hid_t));
  r->file_space_id = (hid_t *)calloc(count, sizeof(hid_t));
  if (plist_id > 0)
    r->xfer_plist_id = H5Pcopy(plist_id);
  for (i = 0; i < count; i++) {
    r->dataset_obj[i] = dset[i];
    if (mem_type_id[i] > 0)
      r->mem_type_id[i] = H5Tcopy(mem_type_id[i]);
    if (mem_space_id[i] > 0)
      r->mem_space_id[i] = H5Scopy(mem_space_id[i]);
    if (file_space_id[i] > 0)
      r->file_space_id[i] = H5Scopy(file_space_id[i]);
  }
  /* set whether to pause async execution */
  H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)o->parent;
  while (p->parent != NULL)
    p = (H5VL_cache_ext_t *)p->parent;
  H5Pset_dxpl_pause(o->H5DWMM->io->request_list->xfer_plist_id, p->async_pause);
  o->H5DWMM->io->request_list->size = size;
  return SUCCEED;
}
#else
static herr_t add_current_write_task_to_queue(void *dset, hid_t mem_type_id,
                                              hid_t mem_space_id,
                                              hid_t file_space_id,
                                              hid_t plist_id, const void *buf) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
#ifndef NDEBUG
  if (debug_level() > 1 && o->H5DWMM->mpi->rank == io_node())
    printf(" [CACHE VOL] Adding current write task %d to queue\n",
           o->H5DWMM->io->request_list->id);
#endif
  o->H5DWMM->io->request_list->buf = o->H5LS->cache_io_cls->write_data_to_cache(
      dset, mem_type_id, mem_space_id, file_space_id, plist_id, buf, NULL);

  hsize_t size = get_buf_size(mem_space_id, mem_type_id);
  // building request list
  o->H5DWMM->io->request_list->offset = o->H5DWMM->mmap->offset;

  o->H5DWMM->mmap->offset += round_page(size);

  o->H5DWMM->cache->mspace_per_rank_left =
      o->H5DWMM->cache->mspace_per_rank_left - round_page(size);

#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank)
    printf(
        " [CACHE VOL] offset, space left (per rank), total storage (per rank) "
        "%llu, %llu, %llu\n",
        o->H5DWMM->mmap->offset, o->H5DWMM->cache->mspace_per_rank_left,
        o->H5DWMM->cache->mspace_per_rank_total);
#endif

  o->H5DWMM->io->request_list->dataset_obj = dset;
  o->H5DWMM->io->request_list->mem_type_id = H5Tcopy(mem_type_id);
  hsize_t ldims[1] = {H5Sget_select_npoints(mem_space_id)};
  o->H5DWMM->io->request_list->mem_space_id = H5Screate_simple(1, ldims, NULL);
  o->H5DWMM->io->request_list->file_space_id = H5Scopy(file_space_id);
  o->H5DWMM->io->request_list->xfer_plist_id = H5Pcopy(plist_id);

  /* set whether to pause async execution */
  H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)o->parent;
  while (p->parent != NULL)
    p = (H5VL_cache_ext_t *)p->parent;
  H5Pset_dxpl_pause(o->H5DWMM->io->request_list->xfer_plist_id, p->async_pause);
  o->H5DWMM->io->request_list->size = size;
  return SUCCEED;
}
#endif

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_write
 *
 * Purpose:     Writes data elements from a buffer into a dataset.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */

#if H5_VERSION_GE(1, 13, 3)
static herr_t
H5VL_cache_ext_dataset_write(size_t count, void *dset[], hid_t mem_type_id[],
                             hid_t mem_space_id[], hid_t file_space_id[],
                             hid_t plist_id, const void *buf[], void **req) {
#ifndef NDEBUG
  if (RANK == io_node())
    printf("------- EXT CACHE VOL DATASET Write\n");
#endif
  void *obj_local;
  void **obj = &obj_local;
  size_t i;
  herr_t ret_value;

  if (count > 1)
    if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
      return -1;

  for (i = 0; i < count; i++) {
    /* Get the object */
    obj[i] = ((H5VL_cache_ext_t *)dset[i])->under_object;

    /* Make sure the class matches */
    if (((H5VL_cache_ext_t *)dset[i])->under_vol_id !=
        ((H5VL_cache_ext_t *)dset[0])->under_vol_id)
      return -1;
  }
  if (((H5VL_cache_ext_t *)dset[0])->write_cache) {
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      // int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
      H5VL_async_set_delay_time(0);
    }
    hsize_t size = 0;

    H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset[0];
    for (i = 0; i < count; i++)
      size += get_buf_size(mem_space_id[i], mem_type_id[i]);
    // Wait for previous request to finish if there is not enough space (notice
    // that we don't need to wait for all the task to finish) write the buffer
    // to the node-local storage
    if (free_cache_space_from_dataset(dset[0], size) < 0) {
#ifndef NDEBUG
      if (((H5VL_cache_ext_t *)dset[i])->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **WARNING: Directly writing data to the storage "
               "layer below\n");
#endif
      ret_value = H5VLdataset_write(
          count, obj, ((H5VL_cache_ext_t *)dset[0])->under_vol_id, mem_type_id,
          mem_space_id, file_space_id, plist_id, buf, req);
      if (req && *req)
        *req = H5VL_cache_ext_new_obj(
            *req, ((H5VL_cache_ext_t *)dset[0])->under_vol_id);
      return ret_value;
    }
    ret_value = add_current_write_task_to_queue(
        count, dset, mem_type_id, mem_space_id, file_space_id, plist_id, buf);
    // calling underlying VOL, assuming the underlying H5VLdataset_write is
    // async
#ifndef NDEBUG
    if (debug_level() > 1 && io_node() == RANK)
      printf(" [CACHE VOL] added task %d to queue\n",
             o->H5DWMM->io->request_list->id);
#endif
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
      H5VL_async_set_delay_time(delay_time);
    }
    ret_value = o->H5LS->cache_io_cls->flush_data_from_cache(
        count, dset, req); // flush data for current task;
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      H5VL_async_set_delay_time(0);
    }
  } else {
    ret_value = H5VLdataset_write(
        count, obj, ((H5VL_cache_ext_t *)dset[0])->under_vol_id, mem_type_id,
        mem_space_id, file_space_id, plist_id, buf, req);
  }
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req,
                                  ((H5VL_cache_ext_t *)dset[0])->under_vol_id);
  if (obj != &obj_local)
    free(obj);
  return ret_value;
}
#else
static herr_t H5VL_cache_ext_dataset_write(void *dset, hid_t mem_type_id,
                                           hid_t mem_space_id,
                                           hid_t file_space_id, hid_t plist_id,
                                           const void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;
#ifndef NDEBUG
  if (RANK == io_node())
    printf("------- EXT CACHE VOL DATASET Write\n");
#endif

  if (o->write_cache) {
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      // int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
      H5VL_async_set_delay_time(0);
    }
    hsize_t size = get_buf_size(mem_space_id, mem_type_id);
    // Wait for previous request to finish if there is not enough space (notice
    // that we don't need to wait for all the task to finish) write the buffer
    // to the node-local storage
    if (free_cache_space_from_dataset(dset, size) < 0) {
#ifndef NDEBUG
      if (o->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] **WARNING: Directly writing data to the storage "
               "layer below\n");
#endif
      ret_value =
          H5VLdataset_write(o->under_object, o->under_vol_id, mem_type_id,
                            mem_space_id, file_space_id, plist_id, buf, req);
      if (req && *req)
        *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
      return ret_value;
    }
    ret_value = add_current_write_task_to_queue(
        (void *)o, mem_type_id, mem_space_id, file_space_id, plist_id, buf);
    // calling underlying VOL, assuming the underlying H5VLdataset_write is
    // async
#ifndef NDEBUG
    if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank)
      printf(" [CACHE VOL] added task %d to queue\n",
             o->H5DWMM->io->request_list->id);
#endif
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
      H5VL_async_set_delay_time(delay_time);
    }
    ret_value = o->H5LS->cache_io_cls->flush_data_from_cache(
        dset, req); // flush data for current task;
    if (getenv("HDF5_ASYNC_DELAY_TIME")) {
      H5VL_async_set_delay_time(0);
    }
  } else {
    ret_value =
        H5VLdataset_write(o->under_object, o->under_vol_id, mem_type_id,
                          mem_space_id, file_space_id, plist_id, buf, req);
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  }
  /* Check for async request */
  return ret_value;
} /* end H5VL_cache_ext_dataset_write() */
#endif

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_get
 *
 * Purpose:     Gets information about a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_dataset_get(void *dset,
                                         H5VL_dataset_get_args_t *args,
                                         hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Get\n");
#endif

  ret_value =
      H5VLdataset_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_dataset_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_specific
 *
 * Purpose:     Specific operation on a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_cache_ext_dataset_specific(void *obj, H5VL_dataset_specific_args_t *args,
                                hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  hid_t under_vol_id;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL H5Dspecific\n");
#endif

  // Save copy of underlying VOL connector ID and prov helper, in case of
  // refresh destroying the current object
  under_vol_id = o->under_vol_id;

#ifndef NDEBUG
  if (RANK == io_node() && debug_level() > 1) {
    printf(" [CACHE VOL] dataset_speicific type: %d\n", args->op_type);
    printf(" [CACHE VOL] H5VL_DATASET_SET_EXTENT-0\n              "
           "H5VL_DATASET_FLUSH-1\n              H5VL_DATASET_REFRESH-2\n");
  }
#endif
  ret_value = H5VLdataset_specific(o->under_object, o->under_vol_id, args,
                                   dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_dataset_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_optional
 *
 * Purpose:     Perform a connector-specific operation on a dataset
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_dataset_optional(void *obj,
                                              H5VL_optional_args_t *args,
                                              hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Optional\n");
#endif
  /* Sanity check */
  assert(-1 != H5VL_cache_dataset_prefetch_op_g);
  assert(-1 != H5VL_cache_dataset_read_to_cache_op_g);
  assert(-1 != H5VL_cache_dataset_read_from_cache_op_g);
  assert(-1 != H5VL_cache_dataset_mmap_remap_op_g);
  assert(-1 != H5VL_cache_dataset_cache_create_op_g);
  assert(-1 != H5VL_cache_dataset_cache_remove_op_g);
  assert(-1 != H5VL_cache_dataset_cache_async_op_start_op_g);
  assert(-1 != H5VL_cache_dataset_cache_async_op_pause_op_g);

  /* Capture and perform connector-specific operations */
  if (args->op_type == H5VL_cache_dataset_prefetch_op_g) {
    H5VL_cache_ext_dataset_prefetch_args_t *opt_args = args->args;

    ret_value = H5VL_cache_ext_dataset_prefetch(obj, opt_args->file_space_id,
                                                dxpl_id, req);
  } else if (args->op_type == H5VL_cache_dataset_read_to_cache_op_g) {
    H5VL_cache_ext_dataset_read_to_cache_args_t *opt_args = args->args;

    // make sure that the data is cached before read
#if H5_VERSION_GE(1, 13, 3)
    ret_value = H5VL_cache_ext_dataset_read_to_cache(
        1, &obj, &opt_args->mem_type_id, &opt_args->mem_space_id,
        &opt_args->file_space_id, dxpl_id, &opt_args->buf, req);
#else
    ret_value = H5VL_cache_ext_dataset_read_to_cache(
        obj, opt_args->mem_type_id, opt_args->mem_space_id,
        opt_args->file_space_id, dxpl_id, opt_args->buf, req);
#endif
  } else if (args->op_type == H5VL_cache_dataset_read_from_cache_op_g) {
    H5VL_cache_ext_dataset_read_from_cache_args_t *opt_args = args->args;
#if H5_VERSION_GE(1, 13, 3)
    ret_value = o->H5LS->cache_io_cls->read_data_from_cache(
        obj, opt_args->mem_type_id, opt_args->mem_space_id,
        opt_args->file_space_id, dxpl_id, opt_args->buf, req);
#else
    ret_value = o->H5LS->cache_io_cls->read_data_from_cache(
        obj, opt_args->mem_type_id, opt_args->mem_space_id,
        opt_args->file_space_id, dxpl_id, opt_args->buf, req);
#endif
  } else if (args->op_type == H5VL_cache_dataset_mmap_remap_op_g) {
    if (o->read_cache) {
#ifndef NDEBUG
      if (o->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE_VOL] mmap_remap\n");
#endif
      ret_value = H5VL_cache_ext_dataset_mmap_remap(obj);
    }
  } else if (args->op_type == H5VL_cache_dataset_cache_remove_op_g) {
    ret_value = o->H5LS->cache_io_cls->remove_dataset_cache(obj, req);
  } else if (args->op_type == H5VL_cache_dataset_cache_create_op_g) {
    H5VL_cache_ext_dataset_cache_create_args_t *opt_args = args->args;
    dset_args_t dset_args;

    dset_args.type_id = dataset_get_type(o->under_object, o->under_vol_id,
                                         H5P_DATASET_XFER_DEFAULT, NULL);
    dset_args.space_id = dataset_get_space(o->under_object, o->under_vol_id,
                                           H5P_DATASET_XFER_DEFAULT, NULL);
    dset_args.dcpl_id = dataset_get_dcpl(o->under_object, o->under_vol_id,
                                         H5P_DATASET_XFER_DEFAULT, NULL);
    dset_args.dapl_id = dataset_get_dapl(o->under_object, o->under_vol_id,
                                         H5P_DATASET_XFER_DEFAULT, NULL);
    dset_args.dxpl_id = H5Pcopy(dxpl_id);
    dset_args.lcpl_id = H5Pcreate(H5P_LINK_CREATE);
    dset_args.name = opt_args->name;
    // dset_args.loc_params = loc_params;
    ret_value =
        o->H5LS->cache_io_cls->create_dataset_cache(obj, &dset_args, req);

  } else if (args->op_type == H5VL_cache_dataset_cache_async_op_pause_op_g) {
    if (o->write_cache || o->read_cache) {
      o->async_pause = true;
#ifndef NDEBUG
      if (debug_level() > 0 && o->write_cache &&
          o->H5DWMM->mpi->rank == io_node())
        printf(
            " [CACHE VOL] pause executing async operations for the dataset\n");
#endif
    }
  } else if (args->op_type == H5VL_cache_dataset_cache_async_op_start_op_g) {
    if (o->write_cache || o->read_cache) {
      o->async_pause = false;
#ifndef NDEBUG
      if (debug_level() > 0 && o->write_cache &&
          o->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] started executing async operations\n");
#endif
      task_data_t *p = o->H5DWMM->io->current_request;
      while (p->req != NULL) {
#ifndef NDEBUG
        if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 0)
          printf(" [CACHE VOL] starting async job: %d\n", p->id);
#endif
        H5async_start(p->req);
        p = p->next;
      }
    }

  } else
    ret_value = H5VLdataset_optional(o->under_object, o->under_vol_id, args,
                                     dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_dataset_optional() */

static herr_t H5VL_cache_ext_dataset_wait(void *dset) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Optional\n");
#endif
  /* Sanity check */

  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;

  H5VL_class_value_t under_value;
  H5VLget_value(o->under_vol_id, &under_value);
  if (under_value != H5VL_ASYNC_VALUE) {
    if (RANK == io_node())
      printf(" [CACHE_VOL] **WARNING: Do not have Async VOL underneath it, no "
             "async process\n");
    return SUCCEED;
  }

  if (o->write_cache) {
    double available = o->H5DWMM->cache->mspace_per_rank_left;
    H5VL_request_status_t status;
    while ((o->num_request_dataset > 0) &&
           (o->H5DWMM->io->current_request != NULL)) {
      double t0 = MPI_Wtime();
      assert(o->H5DWMM->io->current_request->req != NULL);
      H5async_start(o->H5DWMM->io->current_request->req);
      H5VLrequest_wait(o->H5DWMM->io->current_request->req, o->under_vol_id,
                       INF, &status);
      if (o->H5DWMM->io->current_request->buf != NULL &&
          !(strcmp(o->H5LS->scope, "GLOBAL"))) {
        free(o->H5DWMM->io->current_request->buf);
        o->H5DWMM->io->current_request->buf = NULL;
      }
      double t1 = MPI_Wtime();
#ifndef NDEBUG
      if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank) {
        printf(" [CACHE VOL] **H5VLreqeust_wait time (jobid: %d): %f\n",
               o->H5DWMM->io->current_request->id, t1 - t0);
        printf(" [CACHE VOL] **Task %d finished\n",
               o->H5DWMM->io->current_request->id);
      }
#endif
      o->H5DWMM->io->num_request--;
#if H5_VERSION_GE(1, 13, 3)
      for (size_t i = 0; i < o->H5DWMM->io->current_request->count; i++) {
        H5VL_cache_ext_t *d =
            (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj[i];
        d->num_request_dataset--;
      }
#else
      H5VL_cache_ext_t *d =
          (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj;
      d->num_request_dataset--;
#endif
      available = available + round_page(o->H5DWMM->io->current_request->size);

      o->H5DWMM->io->current_request = o->H5DWMM->io->current_request->next;
    }
    o->H5DWMM->cache->mspace_per_rank_left = available;
  }
  if (o->write_cache || o->read_cache) {
    double t0 = MPI_Wtime();
    size_t num_inprogress;
    hbool_t error_occured;
    H5ESwait(o->es_id, INF, &num_inprogress, &error_occured);
    H5ESclose(o->es_id);
    double t1 = MPI_Wtime();
#ifndef NDEBUG
    if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank) {
      printf(" [CACHE VOL] ESwait time: %.5f seconds\n", t1 - t0);
    }
#endif
  }
  return 0;
}

static herr_t H5VL_cache_ext_file_wait(void *file) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  H5VL_class_value_t under_value;
  H5VLget_value(o->under_vol_id, &under_value);
  if (under_value != H5VL_ASYNC_VALUE) {
    if (RANK == io_node())
      printf(" [CACHE VOL] **WARNING: Do not have Async VOL underneath it, no "
             "async process\n");
    return SUCCEED;
  }
#ifndef NDEBUG
  if (io_node() == o->H5DWMM->mpi->rank && debug_level() > 0)
    printf(" [CACHE VOL] file wait\n");
#endif
  if (o->write_cache) {
    double available = o->H5DWMM->cache->mspace_per_rank_left;
    H5VL_request_status_t status;
    while ((o->H5DWMM->io->current_request != NULL) &&
           (o->H5DWMM->io->num_request > 0)) {
#ifndef NDEBUG
      if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank)
        printf(" [CACHE VOL] request wait ...: %d \n",
               o->H5DWMM->io->current_request->id);
#endif
      H5VLrequest_wait(o->H5DWMM->io->current_request->req, o->under_vol_id,
                       INF, &status);
#ifndef NDEBUG
      if (debug_level() > 2 && io_node() == o->H5DWMM->mpi->rank)
        printf(" [CACHE VOL] **Task %d finished\n",
               o->H5DWMM->io->current_request->id);
#endif
      o->H5DWMM->io->num_request--;
#if H5_VERSION_GE(1, 13, 3)
      for (size_t i = 0; i < o->H5DWMM->io->current_request->count; i++) {
        H5VL_cache_ext_t *d =
            (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj[i];
        d->num_request_dataset--;
      }
#else
      H5VL_cache_ext_t *d =
          (H5VL_cache_ext_t *)o->H5DWMM->io->current_request->dataset_obj;
      d->num_request_dataset--;
#endif
      available = available + o->H5DWMM->io->current_request->size;
      o->H5DWMM->io->current_request = o->H5DWMM->io->current_request->next;
    }
    o->H5DWMM->cache->mspace_per_rank_left = available;
  }
  return 0;
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_dataset_close
 *
 * Purpose:     Closes a dataset.
 *
 * Return:      Success:    0
 *              Failure:    -1, dataset not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_dataset_close(void *dset, hid_t dxpl_id,
                                           void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;
  H5VL_cache_ext_t *p = o;
  while (p->parent != NULL)
    p = p->parent;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Close\n");
#endif
  if (p->async_close && o->write_cache) {
    p->async_close_task_list->next =
        (object_close_task_t *)malloc(sizeof(object_close_task_t));
    p->async_close_task_list->type = DATASET_CLOSE;
    p->async_close_task_list->req = NULL;
    p->async_close_task_list->obj = dset;
    ret_value = H5VLdataset_close(o->under_object, o->under_vol_id, dxpl_id,
                                  &p->async_close_task_list->req);
    H5async_start(p->async_close_task_list->req);
    p->async_close_task_list = p->async_close_task_list->next;
    p->async_close_task_list->next = NULL;
    return ret_value;
  }
  double tt0 = MPI_Wtime();
  if (o->read_cache || o->write_cache) {
    double t0 = MPI_Wtime();
    o->H5LS->cache_io_cls->remove_dataset_cache(dset, req);
    double t1 = MPI_Wtime();
#ifndef NDEBUG
    if ((RANK == io_node()) && (debug_level() > 1))
      printf(" [CACHE VOL] dataset remove cache time (including wait time): "
             "%.6f seconds\n",
             t1 - t0);
#endif
  }

  double t0 = MPI_Wtime();
  ret_value = H5VLdataset_close(o->under_object, o->under_vol_id, dxpl_id, req);
  double t1 = MPI_Wtime();
#ifndef NDEBUG
  if (RANK == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] H5VLdataset_close time: %f\n", t1 - t0);
#endif
  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  /* Release our wrapper, if underlying dataset was closed */
  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);
  double tt1 = MPI_Wtime();
#ifndef NDEBUG
  if (RANK == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] H5VL_cache_ext_dataset_close time: %.6f seconds\n",
           tt1 - tt0);
#endif
  return ret_value;
} /* end H5VL_cache_ext_dataset_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_commit
 *
 * Purpose:     Commits a datatype inside a container.
 *
 * Return:      Success:    Pointer to datatype object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_datatype_commit(void *obj,
                                            const H5VL_loc_params_t *loc_params,
                                            const char *name, hid_t type_id,
                                            hid_t lcpl_id, hid_t tcpl_id,
                                            hid_t tapl_id, hid_t dxpl_id,
                                            void **req) {
  H5VL_cache_ext_t *dt;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Commit\n");
#endif

  under =
      H5VLdatatype_commit(o->under_object, loc_params, o->under_vol_id, name,
                          type_id, lcpl_id, tcpl_id, tapl_id, dxpl_id, req);
  if (under) {
    dt = H5VL_cache_ext_new_obj(under, o->under_vol_id);

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    dt = NULL;

  return (void *)dt;
} /* end H5VL_cache_ext_datatype_commit() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_open
 *
 * Purpose:     Opens a named datatype inside a container.
 *
 * Return:      Success:    Pointer to datatype object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_datatype_open(void *obj,
                                          const H5VL_loc_params_t *loc_params,
                                          const char *name, hid_t tapl_id,
                                          hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *dt;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Open\n");
#endif

  under = H5VLdatatype_open(o->under_object, loc_params, o->under_vol_id, name,
                            tapl_id, dxpl_id, req);
  if (under) {
    dt = H5VL_cache_ext_new_obj(under, o->under_vol_id);

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    dt = NULL;

  return (void *)dt;
} /* end H5VL_cache_ext_datatype_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_get
 *
 * Purpose:     Get information about a datatype
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_datatype_get(void *dt,
                                          H5VL_datatype_get_args_t *args,
                                          hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dt;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Get\n");
#endif

  ret_value =
      H5VLdatatype_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_datatype_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_specific
 *
 * Purpose:     Specific operations for datatypes
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_cache_ext_datatype_specific(void *obj, H5VL_datatype_specific_args_t *args,
                                 hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  hid_t under_vol_id;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Specific\n");
#endif

  // Save copy of underlying VOL connector ID and prov helper, in case of
  // refresh destroying the current object
  under_vol_id = o->under_vol_id;

  ret_value = H5VLdatatype_specific(o->under_object, o->under_vol_id, args,
                                    dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_datatype_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_optional
 *
 * Purpose:     Perform a connector-specific operation on a datatype
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_datatype_optional(void *obj,
                                               H5VL_optional_args_t *args,
                                               hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Optional\n");
#endif

  ret_value = H5VLdatatype_optional(o->under_object, o->under_vol_id, args,
                                    dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_datatype_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_datatype_close
 *
 * Purpose:     Closes a datatype.
 *
 * Return:      Success:    0
 *              Failure:    -1, datatype not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_datatype_close(void *dt, hid_t dxpl_id,
                                            void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dt;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATATYPE Close\n");
#endif

  assert(o->under_object);

  ret_value =
      H5VLdatatype_close(o->under_object, o->under_vol_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  /* Release our wrapper, if underlying datatype was closed */
  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);

  return ret_value;
} /* end H5VL_cache_ext_datatype_close() */

char *get_fname(const char *path) {
  char tmp[255];
  strcpy(tmp, path);
  return basename(tmp);
}

static ssize_t file_get_name(void *file, hid_t driver_id, size_t buf_size,
                             char *buf, hid_t dxpl_id, void **req) {
  H5VL_file_get_args_t vol_cb_args;
  size_t file_name_len = 0;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_FILE_GET_NAME;
  vol_cb_args.args.get_name.type = H5I_FILE;
  vol_cb_args.args.get_name.buf_size = buf_size;
  vol_cb_args.args.get_name.buf = buf;
  vol_cb_args.args.get_name.file_name_len = &file_name_len;

  if (H5VLfile_get(file, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return -1;

  return (ssize_t)file_name_len;
}

static hid_t file_get_fapl(void *file, hid_t driver_id, hid_t dxpl_id,
                           void **req) {
  H5VL_file_get_args_t vol_cb_args;

  /* Set up VOL callback arguments */
  vol_cb_args.op_type = H5VL_FILE_GET_FAPL;
  vol_cb_args.args.get_fapl.fapl_id = H5I_INVALID_HID;

  if (H5VLfile_get(file, driver_id, &vol_cb_args, dxpl_id, req) < 0)
    return H5I_INVALID_HID;

  return vol_cb_args.args.get_fapl.fapl_id;
}

/*-------------------------------------------------------------------------
 * Function:    set_file_cache
 *
 * Purpose:     Set file cache on storage, based on environment variables
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t set_file_cache(void *obj, void *file_args, void **req) {
  H5VL_cache_ext_info_t *info;
  H5VL_cache_ext_t *file = (H5VL_cache_ext_t *)obj;
  file_args_t *args = (file_args_t *)file_args;

  H5Pget_vol_info(args->fapl_id, (void **)&info);

  file->write_cache = false;
  file->read_cache = false;
  file->parent = NULL;

  file->H5DRMM = NULL;
  file->H5DWMM = NULL;

  if (getenv("HDF5_CACHE_WR")) {
    if (strcmp(getenv("HDF5_CACHE_WR"), "yes") == 0) {
      file->write_cache = true;
#ifndef NDEBUG
      if (debug_level() > 1 && io_node() == RANK)
        printf(" [CACHE VOL] Write cache turned on for file: %s\n", args->name);
#endif
    }
  } else if (H5Pexist(args->fapl_id, "HDF5_CACHE_WR") > 0) {
    /* Get setting from file property list */
    H5Pget(args->fapl_id, "HDF5_CACHE_WR", &file->write_cache);
  }

  if (getenv("HDF5_CACHE_RD")) {
    if (strcmp(getenv("HDF5_CACHE_RD"), "yes") == 0)
      file->read_cache = true;
#ifndef NDEBUG
    if (debug_level() > 1 && io_node() == RANK)
      printf(" [CACHE VOL] Read cache turned on for file: %s\n", args->name);
#endif
  } else if (H5Pexist(args->fapl_id, "HDF5_CACHE_RD") > 0) {
    H5Pget(args->fapl_id, "HDF5_CACHE_RD", &file->read_cache);
  }

  if (getenv("HDF5_CACHE_DELAY_CLOSE") &&
      (strcmp(getenv("HDF5_CACHE_DELAY_CLOSE"), "yes") == 0)) {
    file->async_close = true;
    file->async_close_task_list =
        (object_close_task_t *)malloc(sizeof(object_close_task_t));
    file->async_close_task_list->next = NULL;
    file->async_close_task_current = file->async_close_task_list;
  }

  file->H5LS = get_cache_storage_obj(info);
  if (file->read_cache || file->write_cache) {
    herr_t ret =
        file->H5LS->cache_io_cls->create_file_cache(obj, file_args, req);
    return ret;
  }
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_create
 *
 * Purpose:     Creates a container using this connector
 *
 * Return:      Success:    Pointer to a file object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_file_create(const char *name, unsigned flags,
                                        hid_t fcpl_id, hid_t fapl_id,
                                        hid_t dxpl_id, void **req) {
  H5VL_cache_ext_info_t *info;
  H5VL_cache_ext_t *file;
  hid_t under_fapl_id;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL FILE Create\n");
#endif
  /* Get copy of our VOL info from FAPL */
  H5Pget_vol_info(fapl_id, (void **)&info);
  /* Make sure we have info about the underlying VOL to be used */
  if (!info)
    return NULL;

  // creating file args
  file_args_t *args = (file_args_t *)malloc(sizeof(file_args_t));
  args->name = name;

  args->fapl_id = fapl_id;
  args->fcpl_id = fcpl_id;
  args->dxpl_id = dxpl_id;
  args->flags = flags;

  /* Copy the FAPL */
  under_fapl_id = H5Pcopy(fapl_id);

  /* Set the VOL ID and info for the underlying FAPL */
  H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info);

  /* Open the file with the underlying VOL connector */
  under = H5VLfile_create(name, flags, fcpl_id, under_fapl_id, dxpl_id, req);
  if (under) {
    file = H5VL_cache_ext_new_obj(under, info->under_vol_id);
    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, info->under_vol_id);
  } /* end if */
  else
    file = NULL;
  file->async_pause = false;

  /* Set file cache information */
  set_file_cache((void *)file, (void *)args, req);
  free(args);

  /* Close underlying FAPL */
  H5Pclose(under_fapl_id);

  /* Release copy of our VOL info */
  H5VL_cache_ext_info_free(info);
  return (void *)file;
} /* end H5VL_cache_ext_file_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_open
 *
 * Purpose:     Opens a container created with this connector
 *
 * Return:      Success:    Pointer to a file object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_file_open(const char *name, unsigned flags,
                                      hid_t fapl_id, hid_t dxpl_id,
                                      void **req) {
  H5VL_cache_ext_info_t *info;
  H5VL_cache_ext_t *file;
  hid_t under_fapl_id;
  void *under;
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL FILE Open\n");
#endif

  /* Get copy of our VOL info from FAPL */
  H5Pget_vol_info(fapl_id, (void **)&info);
  /* Make sure we have info about the underlying VOL to be used */
  if (!info)
    return NULL;

  file_args_t *args = (file_args_t *)malloc(sizeof(file_args_t));
  args->name = name;
  args->fapl_id = fapl_id;
  args->fcpl_id = H5Pcreate(H5P_FILE_CREATE);
  args->dxpl_id = dxpl_id;
  args->flags = flags;

  /* Copy the FAPL */
  under_fapl_id = H5Pcopy(fapl_id);

  /* Set the VOL ID and info for the underlying FAPL */
  H5Pset_vol(under_fapl_id, info->under_vol_id, info->under_vol_info);

  /* Open the file with the underlying VOL connector */
  under = H5VLfile_open(name, flags, under_fapl_id, dxpl_id, req);
  if (under) {
    file = H5VL_cache_ext_new_obj(under, info->under_vol_id);

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, info->under_vol_id);
    /* turn on read, only when MPI is initialized. This is to solve some issue
     * in h5dump, h5ls apps */
  } else
    file = NULL;

  /* do not pause async execution */
  file->async_pause = false;
  set_file_cache((void *)file, (void *)args, req);
  free(args);
  /* Close underlying FAPL */
  H5Pclose(under_fapl_id);

  H5VL_cache_ext_info_free(info);
  return (void *)file;
} /* end H5VL_cache_ext_file_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_get
 *
 * Purpose:     Get info about a file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_file_get(void *file, H5VL_file_get_args_t *args,
                                      hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL FILE Get\n");
#endif

  ret_value =
      H5VLfile_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_file_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_specific
 *
 * Purpose:     Specific operation on file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_file_specific(void *file,
                                           H5VL_file_specific_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  H5VL_cache_ext_t *new_o;
  H5VL_file_specific_args_t my_args;
  H5VL_file_specific_args_t *new_args;
  H5VL_cache_ext_info_t *info;
  hid_t under_vol_id = -1;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL FILE Specific\n");
#endif

  /* Check for 'is accessible' operation */
  if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
    /* Make a (shallow) copy of the arguments */
    memcpy(&my_args, args, sizeof(my_args));

    /* Set up the new FAPL for the updated arguments */

    /* Get copy of our VOL info from FAPL */
    H5Pget_vol_info(args->args.is_accessible.fapl_id, (void **)&info);

    /* Make sure we have info about the underlying VOL to be used */
    if (!info)
      return (-1);

    /* Keep the correct underlying VOL ID for later */
    under_vol_id = info->under_vol_id;

    /* Copy the FAPL */
    my_args.args.is_accessible.fapl_id =
        H5Pcopy(args->args.is_accessible.fapl_id);

    /* Set the VOL ID and info for the underlying FAPL */
    H5Pset_vol(my_args.args.is_accessible.fapl_id, info->under_vol_id,
               info->under_vol_info);

    /* Set argument pointer to new arguments */
    new_args = &my_args;

    /* Set object pointer for operation */
    new_o = NULL;
  } /* end else-if */
  /* Check for 'delete' operation */
  else if (args->op_type == H5VL_FILE_DELETE) {
    /* Make a (shallow) copy of the arguments */
    memcpy(&my_args, args, sizeof(my_args));

    /* Set up the new FAPL for the updated arguments */

    /* Get copy of our VOL info from FAPL */
    H5Pget_vol_info(args->args.del.fapl_id, (void **)&info);

    /* Make sure we have info about the underlying VOL to be used */
    if (!info)
      return (-1);

    /* Keep the correct underlying VOL ID for later */
    under_vol_id = info->under_vol_id;

    /* Copy the FAPL */
    my_args.args.del.fapl_id = H5Pcopy(args->args.del.fapl_id);

    /* Set the VOL ID and info for the underlying FAPL */
    H5Pset_vol(my_args.args.del.fapl_id, info->under_vol_id,
               info->under_vol_info);

    /* Set argument pointer to new arguments */
    new_args = &my_args;

    /* Set object pointer for operation */
    new_o = NULL;
  } /* end else-if */
  else {
    /* Keep the correct underlying VOL ID for later */
    under_vol_id = o->under_vol_id;

    /* Set argument pointer to current arguments */
    new_args = args;

    /* Set object pointer for operation */
    new_o = o->under_object;
  } /* end else */

  ret_value = H5VLfile_specific(new_o, under_vol_id, new_args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  /* Check for 'is accessible' operation */
  if (args->op_type == H5VL_FILE_IS_ACCESSIBLE) {
    /* Close underlying FAPL */
    H5Pclose(my_args.args.is_accessible.fapl_id);

    /* Release copy of our VOL info */
    H5VL_cache_ext_info_free(info);
  } /* end else-if */
  /* Check for 'delete' operation */
  else if (args->op_type == H5VL_FILE_DELETE) {
    /* Close underlying FAPL */
    H5Pclose(my_args.args.del.fapl_id);

    /* Release copy of our VOL info */
    H5VL_cache_ext_info_free(info);
  } /* end else-if */
  else if (args->op_type == H5VL_FILE_REOPEN) {
    /* Wrap reopened file struct pointer, if we reopened one */
    if (ret_value >= 0 && args->args.reopen.file)
      *args->args.reopen.file =
          H5VL_cache_ext_new_obj(*args->args.reopen.file, o->under_vol_id);
  } /* end else */

  return ret_value;
} /* end H5VL_cache_ext_file_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_optional
 *
 * Purpose:     Perform a connector-specific operation on a file
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_file_optional(void *file,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL File Optional\n");
#endif
  assert(-1 != H5VL_cache_file_cache_create_op_g);
  assert(-1 != H5VL_cache_file_cache_remove_op_g);
  assert(-1 != H5VL_cache_file_cache_async_op_start_op_g);
  assert(-1 != H5VL_cache_file_cache_async_op_pause_op_g);
  assert(-1 != H5VL_cache_file_cache_async_close_set_op_g);
  assert(-1 != H5VL_cache_file_cache_async_close_wait_op_g);

  if (args->op_type == H5VL_cache_file_cache_create_op_g) {
    H5VL_cache_ext_file_cache_create_args_t *opt_args = args->args;

    if (opt_args->purpose == WRITE)
      o->write_cache = true;
    else if (opt_args->purpose == READ)
      o->read_cache = true;
    else if (opt_args->purpose == RDWR) {
      o->read_cache = true;
      o->write_cache = true;
    }

    if (o->write_cache || o->read_cache) {
      file_args_t file_args;
      char name[255];
#ifndef NDEBUG
      if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] file optional: file cache create\n");
#endif
      file_get_name(o->under_object, o->under_vol_id, sizeof(name), name,
                    H5P_DATASET_XFER_DEFAULT, H5_REQUEST_NULL);
      file_args.name = name;
      file_args.fapl_id = opt_args->fapl_id;
      ret_value =
          o->H5LS->cache_io_cls->create_file_cache(file, &file_args, req);
    }

  } else if (args->op_type == H5VL_cache_file_cache_remove_op_g) {
    if (o->write_cache && o->H5DWMM != NULL) {
#ifndef NDEBUG
      if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] file optional: file cache remove\n");
#endif
      ret_value = H5LSremove_cache(o->H5LS, o->H5DWMM->cache);

      free(o->H5DWMM);
    }

  } else if (args->op_type == H5VL_cache_file_cache_async_op_pause_op_g) {
    if (o->write_cache) {
      // we set the delay time to be 0 since we are pause the tasks explicitly
      //      H5VL_async_set_delay_time(0);
#ifndef NDEBUG
      if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] file optional: file_cache_async_op_pause\n");
#endif
      o->async_pause = true;
#ifndef NDEBUG
      if (debug_level() > 0 && o->write_cache &&
          o->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] pause executing async operations\n");
#endif
    }
    ret_value = SUCCEED;
  } else if (args->op_type == H5VL_cache_file_cache_async_op_start_op_g) {
    if (o->write_cache) {
      //      H5VL_async_set_delay_time(0);
      o->async_pause = false;
#ifndef NDEBUG
      if (debug_level() > 0 && o->write_cache &&
          o->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] started executing async operations\n");
#endif
      task_data_t *p = o->H5DWMM->io->current_request;
      while (p->req != NULL) {
        H5async_start(p->req);
#ifndef NDEBUG
        if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 0)
          printf(" [CACHE VOL] starting async job: %d\n", p->id);
#endif
        p = p->next;
      }
    }
    ret_value = SUCCEED;
  } else if (args->op_type == H5VL_cache_file_cache_async_close_set_op_g) {
#ifndef NDEBUG
    if (RANK == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] set async close\n");
#endif
    o->async_close = true;
    o->async_close_task_list =
        (object_close_task_t *)malloc(sizeof(object_close_task_t));
    o->async_close_task_list->next = NULL;
    o->async_close_task_current = o->async_close_task_list;
    ret_value = SUCCEED;
  } else if (args->op_type == H5VL_cache_file_cache_async_close_wait_op_g) {
#ifndef NDEBUG
    if (RANK == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] async close wait\n");
#endif
    while (o->async_close_task_current->next != NULL) {
      async_close_task_wait(o->async_close_task_current);
      o->async_close_task_current = o->async_close_task_current->next;
    }
    ret_value = SUCCEED;
  } else {
#ifndef NDEBUG
    if (RANK == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] File optional: args->op_type: %d\n", args->op_type);
#endif
    ret_value =
        H5VLfile_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);
  }

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_file_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_file_close
 *
 * Purpose:     Closes a file.
 *
 * Return:      Success:    0
 *              Failure:    -1, file not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_file_close(void *file, hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL FILE Close\n");
#endif
  if (o->async_close && o->write_cache) {
    while (o->async_close_task_current->next != NULL) {
      async_close_task_wait(o->async_close_task_current);
      o->async_close_task_current = o->async_close_task_current->next;
#ifndef NDEBUG
      if (RANK == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] delay close object: %d\n",
               o->async_close_task_current->type);
#endif
    }
  }
  if (o->read_cache || o->write_cache)
    o->H5LS->cache_io_cls->remove_file_cache(file, req);

  ret_value = H5VLfile_close(o->under_object, o->under_vol_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  /* Release our wrapper, if underlying file was closed */
  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);

  return ret_value;
} /* end H5VL_cache_ext_file_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_create
 *
 * Purpose:     Creates a group inside a container
 *
 * Return:      Success:    Pointer to a group object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_group_create(void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         const char *name, hid_t lcpl_id,
                                         hid_t gcpl_id, hid_t gapl_id,
                                         hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *group;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Create\n");
#endif

  under = H5VLgroup_create(o->under_object, loc_params, o->under_vol_id, name,
                           lcpl_id, gcpl_id, gapl_id, dxpl_id, req);
  if (under) {
    group = H5VL_cache_ext_new_obj(under, o->under_vol_id);
    /* passing the cache information on from file to group */
    group->write_cache = o->write_cache;
    group->read_cache = o->read_cache;
    group->parent = obj;
    group->H5LS = o->H5LS;
    group->async_pause = o->async_pause;
    if (group->write_cache || group->read_cache) {
      group_args_t *args = (group_args_t *)malloc(sizeof(group_args_t));
      args->loc_params = loc_params;
      args->name = name;
      args->lcpl_id = lcpl_id;
      args->gcpl_id = gcpl_id;
      args->gapl_id = gapl_id;
      args->dxpl_id = dxpl_id;
      group->H5LS->cache_io_cls->create_group_cache((void *)group, (void *)args,
                                                    req);
      free(args);
    }
    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    group = NULL;

  return (void *)group;
} /* end H5VL_cache_ext_group_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_open
 *
 * Purpose:     Opens a group inside a container
 *
 * Return:      Success:    Pointer to a group object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_group_open(void *obj,
                                       const H5VL_loc_params_t *loc_params,
                                       const char *name, hid_t gapl_id,
                                       hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *group;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Open\n");
#endif

  under = H5VLgroup_open(o->under_object, loc_params, o->under_vol_id, name,
                         gapl_id, dxpl_id, req);
  if (under) {
    group = H5VL_cache_ext_new_obj(under, o->under_vol_id);
    /* passing the cache information on from file to group */
    group->write_cache = o->write_cache;
    group->H5DWMM = o->H5DWMM;
    group->read_cache = o->read_cache;
    group->H5DRMM = o->H5DRMM;
    group->parent = obj;
    group->H5LS = o->H5LS;
    group->async_pause = o->async_pause;
    if (group->write_cache || group->read_cache) {
      group_args_t *args = (group_args_t *)malloc(sizeof(group_args_t));
      args->lcpl_id = H5Pcreate(H5P_LINK_CREATE);
      args->loc_params = loc_params;
      args->name = name;
      args->gcpl_id = H5Pcreate(H5P_GROUP_CREATE);
      args->gapl_id = gapl_id;
      args->dxpl_id = dxpl_id;
      group->H5LS->cache_io_cls->create_group_cache((void *)group, (void *)args,
                                                    req);
      free(args);
    }

    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    group = NULL;

  return (void *)group;
} /* end H5VL_cache_ext_group_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_get
 *
 * Purpose:     Get info about a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_group_get(void *obj, H5VL_group_get_args_t *args,
                                       hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Get\n");
#endif

  ret_value =
      H5VLgroup_get(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_group_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_specific
 *
 * Purpose:     Specific operation on a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_group_specific(void *obj,
                                            H5VL_group_specific_args_t *args,
                                            hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  H5VL_group_specific_args_t my_args;
  H5VL_group_specific_args_t *new_args;
  hid_t under_vol_id;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Specific\n");
#endif

  // Save copy of underlying VOL connector ID and prov helper, in case of
  // refresh destroying the current object
  under_vol_id = o->under_vol_id;

  /* Unpack arguments to get at the child file pointer when mounting a file */
  if (args->op_type == H5VL_GROUP_MOUNT) {

    /* Make a (shallow) copy of the arguments */
    memcpy(&my_args, args, sizeof(my_args));

    /* Set the object for the child file */
    my_args.args.mount.child_file =
        ((H5VL_cache_ext_t *)args->args.mount.child_file)->under_object;

    /* Point to modified arguments */
    new_args = &my_args;
  } /* end if */
  else
    new_args = args;

  ret_value =
      H5VLgroup_specific(o->under_object, under_vol_id, new_args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_group_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_optional
 *
 * Purpose:     Perform a connector-specific operation on a group
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_group_optional(void *obj,
                                            H5VL_optional_args_t *args,
                                            hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Optional\n");
#endif

  ret_value =
      H5VLgroup_optional(o->under_object, o->under_vol_id, args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_group_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_group_close
 *
 * Purpose:     Closes a group.
 *
 * Return:      Success:    0
 *              Failure:    -1, group not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_group_close(void *grp, hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)grp;
  herr_t ret_value;

  H5VL_cache_ext_t *p = o;
  while (p->parent != NULL)
    p = p->parent;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL GROUP Close\n");
#endif
  if (p->async_close && o->write_cache) {
    p->async_close_task_list->next =
        (object_close_task_t *)malloc(sizeof(object_close_task_t));
    p->async_close_task_list->type = GROUP_CLOSE;
    p->async_close_task_list->req = NULL;
    p->async_close_task_list->obj = grp;
    ret_value = H5VLgroup_close(o->under_object, o->under_vol_id, dxpl_id,
                                &p->async_close_task_list->req);
    H5async_start(p->async_close_task_list->req);
    p->async_close_task_list = p->async_close_task_list->next;
    p->async_close_task_list->next = NULL;
    return ret_value;
  }
  if (o->read_cache || o->write_cache)
    o->H5LS->cache_io_cls->remove_group_cache(grp, req);

  ret_value = H5VLgroup_close(o->under_object, o->under_vol_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  /* Release our wrapper, if underlying file was closed */
  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);

  return ret_value;
} /* end H5VL_cache_ext_group_close() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_create
 *
 * Purpose:     Creates a hard / soft / UD / external link.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_create(H5VL_link_create_args_t *args,
                                         void *obj,
                                         const H5VL_loc_params_t *loc_params,
                                         hid_t lcpl_id, hid_t lapl_id,
                                         hid_t dxpl_id, void **req) {
  H5VL_link_create_args_t my_args;
  H5VL_link_create_args_t *new_args;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  hid_t under_vol_id = -1;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Create\n");
#endif

  /* Try to retrieve the "under" VOL id */
  if (o)
    under_vol_id = o->under_vol_id;

  /* Fix up the link target object for hard link creation */
  if (H5VL_LINK_CREATE_HARD == args->op_type) {
    /* If it's a non-NULL pointer, find the 'under object' and re-set the args
     */
    if (args->args.hard.curr_obj) {
      /* Make a (shallow) copy of the arguments */
      memcpy(&my_args, args, sizeof(my_args));

      /* Check if we still need the "under" VOL ID */
      if (under_vol_id < 0)
        under_vol_id =
            ((H5VL_cache_ext_t *)args->args.hard.curr_obj)->under_vol_id;

      /* Set the object for the link target */
      my_args.args.hard.curr_obj =
          ((H5VL_cache_ext_t *)args->args.hard.curr_obj)->under_object;

      /* Set argument pointer to modified parameters */
      new_args = &my_args;
    } /* end if */
    else
      new_args = args;
  } /* end if */
  else
    new_args = args;

  /* Re-issue 'link create' call, possibly using the unwrapped pieces */
  ret_value =
      H5VLlink_create(new_args, (o ? o->under_object : NULL), loc_params,
                      under_vol_id, lcpl_id, lapl_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_create() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_copy
 *
 * Purpose:     Renames an object within an HDF5 container and copies it to a
 *new group.  The original name SRC is unlinked from the group graph and then
 *inserted with the new name DST (which can specify a new path for the object)
 *as an atomic operation. The names are interpreted relative to SRC_LOC_ID and
 *              DST_LOC_ID, which are either file IDs or group ID.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_copy(void *src_obj,
                                       const H5VL_loc_params_t *loc_params1,
                                       void *dst_obj,
                                       const H5VL_loc_params_t *loc_params2,
                                       hid_t lcpl_id, hid_t lapl_id,
                                       hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o_src = (H5VL_cache_ext_t *)src_obj;
  H5VL_cache_ext_t *o_dst = (H5VL_cache_ext_t *)dst_obj;
  hid_t under_vol_id = -1;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Copy\n");
#endif

  /* Retrieve the "under" VOL id */
  if (o_src)
    under_vol_id = o_src->under_vol_id;
  else if (o_dst)
    under_vol_id = o_dst->under_vol_id;
  assert(under_vol_id > 0);

  ret_value = H5VLlink_copy((o_src ? o_src->under_object : NULL), loc_params1,
                            (o_dst ? o_dst->under_object : NULL), loc_params2,
                            under_vol_id, lcpl_id, lapl_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_move
 *
 * Purpose:     Moves a link within an HDF5 file to a new group.  The original
 *              name SRC is unlinked from the group graph
 *              and then inserted with the new name DST (which can specify a
 *              new path for the object) as an atomic operation. The names
 *              are interpreted relative to SRC_LOC_ID and
 *              DST_LOC_ID, which are either file IDs or group ID.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_move(void *src_obj,
                                       const H5VL_loc_params_t *loc_params1,
                                       void *dst_obj,
                                       const H5VL_loc_params_t *loc_params2,
                                       hid_t lcpl_id, hid_t lapl_id,
                                       hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o_src = (H5VL_cache_ext_t *)src_obj;
  H5VL_cache_ext_t *o_dst = (H5VL_cache_ext_t *)dst_obj;
  hid_t under_vol_id = -1;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Move\n");
#endif

  /* Retrieve the "under" VOL id */
  if (o_src)
    under_vol_id = o_src->under_vol_id;
  else if (o_dst)
    under_vol_id = o_dst->under_vol_id;
  assert(under_vol_id > 0);

  ret_value = H5VLlink_move((o_src ? o_src->under_object : NULL), loc_params1,
                            (o_dst ? o_dst->under_object : NULL), loc_params2,
                            under_vol_id, lcpl_id, lapl_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_move() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_get
 *
 * Purpose:     Get info about a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_get(void *obj,
                                      const H5VL_loc_params_t *loc_params,
                                      H5VL_link_get_args_t *args, hid_t dxpl_id,
                                      void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Get\n");
#endif

  ret_value = H5VLlink_get(o->under_object, loc_params, o->under_vol_id, args,
                           dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_specific
 *
 * Purpose:     Specific operation on a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_specific(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_link_specific_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Specific\n");
#endif

  ret_value = H5VLlink_specific(o->under_object, loc_params, o->under_vol_id,
                                args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_link_optional
 *
 * Purpose:     Perform a connector-specific operation on a link
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_link_optional(void *obj,
                                           const H5VL_loc_params_t *loc_params,
                                           H5VL_optional_args_t *args,
                                           hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL LINK Optional\n");
#endif

  ret_value = H5VLlink_optional(o->under_object, loc_params, o->under_vol_id,
                                args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_link_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_object_open
 *
 * Purpose:     Opens an object inside a container.
 *
 * Return:      Success:    Pointer to object
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static void *H5VL_cache_ext_object_open(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        H5I_type_t *opened_type, hid_t dxpl_id,
                                        void **req) {
  H5VL_cache_ext_t *new_obj;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  void *under;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL OBJECT Open\n");
#endif
  under = H5VLobject_open(o->under_object, loc_params, o->under_vol_id,
                          opened_type, dxpl_id, req);

  if (under) {
    new_obj = H5VL_cache_ext_new_obj(under, o->under_vol_id);
    if (*opened_type == H5I_GROUP) { // if group is opened
      new_obj->read_cache = o->read_cache;
      new_obj->write_cache = o->write_cache;
      new_obj->H5DWMM = o->H5DWMM;
      new_obj->H5DRMM = o->H5DRMM;
      new_obj->parent = obj;
      new_obj->H5LS = o->H5LS;
      new_obj->async_close = o->async_close;
#ifndef NDEBUG
      if (debug_level() > 1 && RANK == io_node())
        printf(" [CACHE VOL] object(group) open\n");
#endif
      if (new_obj->write_cache || new_obj->read_cache) {
        group_args_t *args = (group_args_t *)malloc(sizeof(group_args_t));
        args->lcpl_id = H5Pcreate(H5P_LINK_CREATE);
        args->loc_params = loc_params;
        args->name = loc_params->loc_data.loc_by_name.name;
        args->gcpl_id = H5Pcreate(H5P_GROUP_CREATE);
        args->gapl_id =
            group_get_gapl(new_obj->under_object, new_obj->under_vol_id,
                           H5P_DATASET_XFER_DEFAULT, req);
        args->dxpl_id = dxpl_id;
        new_obj->H5LS->cache_io_cls->create_group_cache((void *)new_obj,
                                                        (void *)args, req);
        free(args);
      }
    } else if (*opened_type == H5I_DATASET) { // if dataset is opened
#ifndef NDEBUG
      if (debug_level() > 1 && RANK == io_node())
        printf(" [CACHE VOL] object(dataset) open\n");
#endif
      H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)obj;
      while (p->parent != NULL)
        p = p->parent;
      new_obj->read_cache = p->read_cache;
      new_obj->write_cache = p->write_cache;
      new_obj->async_close = p->async_close;
      new_obj->H5DRMM = o->H5DRMM;
      new_obj->H5DWMM = o->H5DWMM;
      new_obj->parent = obj;
      new_obj->H5LS = o->H5LS;
      new_obj->es_id = H5EScreate();
      int called = 0;
      MPI_Initialized(&called);
      if (new_obj->write_cache) {
#ifndef NDEBUG
        if (debug_level() > 1 && RANK == io_node())
          printf(" [CACHE VOL] object(dataset) write cache on\n");
#endif
      } else {
#ifndef NDEBUG
        if (debug_level() > 1 && RANK == io_node())
          printf(" [CACHE VOL] object(dataset) write cache off\n");
#endif
      }
      if (called && (new_obj->read_cache || new_obj->write_cache)) {
        dset_args_t *args = (dset_args_t *)malloc(sizeof(dset_args_t));
        args->type_id =
            dataset_get_type(new_obj->under_object, new_obj->under_vol_id,
                             H5P_DATASET_XFER_DEFAULT, NULL);
        args->space_id =
            dataset_get_space(new_obj->under_object, new_obj->under_vol_id,
                              H5P_DATASET_XFER_DEFAULT, NULL);
        args->dcpl_id =
            dataset_get_dcpl(new_obj->under_object, new_obj->under_vol_id,
                             H5P_DATASET_XFER_DEFAULT, NULL);
        args->dapl_id =
            dataset_get_dapl(new_obj->under_object, new_obj->under_vol_id,
                             H5P_DATASET_XFER_DEFAULT, NULL);
        args->lcpl_id = H5Pcreate(H5P_LINK_CREATE);
        args->loc_params = loc_params;
        args->name = loc_params->loc_data.loc_by_name.name;
        args->dxpl_id = dxpl_id;

        new_obj->H5LS->cache_io_cls->create_dataset_cache((void *)new_obj,
                                                          (void *)args, req);
        free(args);
        if (getenv("DATASET_PREFETCH_AT_OPEN")) {
          if (new_obj->read_cache &&
              !strcmp(getenv("DATASET_PREFETCH_AT_OPEN"), "yes")) {
#ifndef NDEBUG
            if (debug_level() > 1 && new_obj->H5DRMM->mpi->rank == io_node())
              printf(" [CACHE VOL] DATASET_PREFETCH_AT_OPEN = yes\n");
#endif
            H5VL_cache_ext_dataset_prefetch_async(
                new_obj, args->space_id, dxpl_id, new_obj->prefetch_req);
          }
        }
      }
    }
    /* Check for async request */
    if (req && *req)
      *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);
  } /* end if */
  else
    new_obj = NULL;

  return (void *)new_obj;
} /* end H5VL_cache_ext_object_open() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_object_copy
 *
 * Purpose:     Copies an object inside a container.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_object_copy(
    void *src_obj, const H5VL_loc_params_t *src_loc_params,
    const char *src_name, void *dst_obj,
    const H5VL_loc_params_t *dst_loc_params, const char *dst_name,
    hid_t ocpypl_id, hid_t lcpl_id, hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o_src = (H5VL_cache_ext_t *)src_obj;
  H5VL_cache_ext_t *o_dst = (H5VL_cache_ext_t *)dst_obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL OBJECT Copy\n");
#endif

  ret_value =
      H5VLobject_copy(o_src->under_object, src_loc_params, src_name,
                      o_dst->under_object, dst_loc_params, dst_name,
                      o_src->under_vol_id, ocpypl_id, lcpl_id, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o_src->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_object_copy() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_object_get
 *
 * Purpose:     Get info about an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_object_get(void *obj,
                                        const H5VL_loc_params_t *loc_params,
                                        H5VL_object_get_args_t *args,
                                        hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL OBJECT Get\n");
#endif

  ret_value = H5VLobject_get(o->under_object, loc_params, o->under_vol_id, args,
                             dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_object_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_object_specific
 *
 * Purpose:     Specific operation on an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_cache_ext_object_specific(void *obj, const H5VL_loc_params_t *loc_params,
                               H5VL_object_specific_args_t *args, hid_t dxpl_id,
                               void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  hid_t under_vol_id;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL OBJECT Specific\n");
#endif

  // Save copy of underlying VOL connector ID and prov helper, in case of
  // refresh destroying the current object
  under_vol_id = o->under_vol_id;

  ret_value = H5VLobject_specific(o->under_object, loc_params, o->under_vol_id,
                                  args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_object_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_object_optional
 *
 * Purpose:     Perform a connector-specific operation for an object
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_cache_ext_object_optional(void *obj, const H5VL_loc_params_t *loc_params,
                               H5VL_optional_args_t *args, hid_t dxpl_id,
                               void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL OBJECT Optional\n");
#endif

  ret_value = H5VLobject_optional(o->under_object, loc_params, o->under_vol_id,
                                  args, dxpl_id, req);

  /* Check for async request */
  if (req && *req)
    *req = H5VL_cache_ext_new_obj(*req, o->under_vol_id);

  return ret_value;
} /* end H5VL_cache_ext_object_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_introspect_get_conn_clss
 *
 * Purpose:     Query the connector class.
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_introspect_get_conn_cls(void *obj,
                                              H5VL_get_conn_lvl_t lvl,
                                              const H5VL_class_t **conn_cls) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INTROSPECT GetConnCls\n");
#endif

  /* Check for querying this connector's class */
  if (H5VL_GET_CONN_LVL_CURR == lvl) {
    *conn_cls = &H5VL_cache_ext_g;
    ret_value = 0;
  } /* end if */
  else
    ret_value = H5VLintrospect_get_conn_cls(o->under_object, o->under_vol_id,
                                            lvl, conn_cls);

  return ret_value;
} /* end H5VL_cache_ext_introspect_get_conn_cls() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_introspect_get_cap_flags
 *
 * Purpose:     Query the capability flags for this connector and any
 *              underlying connector(s).
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_introspect_get_cap_flags(const void *_info,
                                               unsigned *cap_flags) {
  const H5VL_cache_ext_info_t *info = (const H5VL_cache_ext_info_t *)_info;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INTROSPECT GetCapFlags\n");
#endif

  /* Invoke the query on the underlying VOL connector */
  ret_value = H5VLintrospect_get_cap_flags(info->under_vol_info,
                                           info->under_vol_id, cap_flags);

  /* Bitwise OR our capability flags in */
  if (ret_value >= 0)
    *cap_flags |= H5VL_cache_ext_g.cap_flags;

  return ret_value;
} /* end H5VL_cache_ext_introspect_ext_get_cap_flags() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_introspect_opt_query
 *
 * Purpose:     Query if an optional operation is supported by this connector
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_introspect_opt_query(void *obj, H5VL_subclass_t cls,
                                           int opt_type, uint64_t *flags) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL INTROSPECT OptQuery\n");
#endif

  ret_value = H5VLintrospect_opt_query(o->under_object, o->under_vol_id, cls,
                                       opt_type, flags);

  return ret_value;
} /* end H5VL_cache_ext_introspect_opt_query() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_notify
 *
 * Purpose:     Registers a user callback to be invoked when an asynchronous
 *              operation completes
 *
 * Note:        Releases the request, if connector callback succeeds
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_request_notify(void *obj, H5VL_request_notify_t cb,
                                            void *ctx) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Notify\n");
#endif

  ret_value = H5VLrequest_notify(o->under_object, o->under_vol_id, cb, ctx);

  return ret_value;
} /* end H5VL_cache_ext_request_notify() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_cancel
 *
 * Purpose:     Cancels an asynchronous operation
 *
 * Note:        Releases the request, if connector callback succeeds
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_request_cancel(void *obj,
                                            H5VL_request_status_t *status) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Cancel\n");
#endif

  ret_value = H5VLrequest_cancel(o->under_object, o->under_vol_id, status);

  return ret_value;
} /* end H5VL_cache_ext_request_cancel() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_specific
 *
 * Purpose:     Specific operation on a request
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL_cache_ext_request_specific(void *obj, H5VL_request_specific_args_t *args) {
  herr_t ret_value = -1;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Specific\n");
#endif
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;

  ret_value = H5VLrequest_specific(o->under_object, o->under_vol_id, args);

  return ret_value;
} /* end H5VL_cache_ext_request_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_optional
 *
 * Purpose:     Perform a connector-specific operation for a request
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_request_optional(void *obj,
                                              H5VL_optional_args_t *args) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Optional\n");
#endif

  ret_value = H5VLrequest_optional(o->under_object, o->under_vol_id, args);

  return ret_value;
} /* end H5VL_cache_ext_request_optional() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_request_free
 *
 * Purpose:     Releases a request, allowing the operation to complete without
 *              application tracking
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_request_free(void *obj) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL REQUEST Free\n");
#endif

  ret_value = H5VLrequest_free(o->under_object, o->under_vol_id);

  if (ret_value >= 0)
    H5VL_cache_ext_free_obj(o);

  return ret_value;
} /* end H5VL_cache_ext_request_free() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_blob_put
 *
 * Purpose:     Handles the blob 'put' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_blob_put(void *obj, const void *buf, size_t size,
                               void *blob_id, void *ctx) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL BLOB Put\n");
#endif

  ret_value =
      H5VLblob_put(o->under_object, o->under_vol_id, buf, size, blob_id, ctx);

  return ret_value;
} /* end H5VL_cache_ext_blob_put() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_blob_get
 *
 * Purpose:     Handles the blob 'get' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_blob_get(void *obj, const void *blob_id, void *buf,
                               size_t size, void *ctx) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL BLOB Get\n");
#endif

  ret_value =
      H5VLblob_get(o->under_object, o->under_vol_id, blob_id, buf, size, ctx);

  return ret_value;
} /* end H5VL_cache_ext_blob_get() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_blob_specific
 *
 * Purpose:     Handles the blob 'specific' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_blob_specific(void *obj, void *blob_id,
                                    H5VL_blob_specific_args_t *args) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL BLOB Specific\n");
#endif

  ret_value =
      H5VLblob_specific(o->under_object, o->under_vol_id, blob_id, args);

  return ret_value;
} /* end H5VL_cache_ext_blob_specific() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_blob_optional
 *
 * Purpose:     Handles the blob 'optional' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_blob_optional(void *obj, void *blob_id,
                                    H5VL_optional_args_t *args) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL BLOB Optional\n");
#endif

  ret_value =
      H5VLblob_optional(o->under_object, o->under_vol_id, blob_id, args);

  return ret_value;
} /* end H5VL_cache_ext_blob_optional() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_token_cmp
 *
 * Purpose:     Compare two of the connector's object tokens, setting
 *              *cmp_value, following the same rules as strcmp().
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_token_cmp(void *obj, const H5O_token_t *token1,
                                       const H5O_token_t *token2,
                                       int *cmp_value) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL TOKEN Compare\n");
#endif

  /* Sanity checks */
  assert(obj);
  assert(token1);
  assert(token2);
  assert(cmp_value);

  ret_value = H5VLtoken_cmp(o->under_object, o->under_vol_id, token1, token2,
                            cmp_value);

  return ret_value;
} /* end H5VL_cache_ext_token_cmp() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_token_to_str
 *
 * Purpose:     Serialize the connector's object token into a string.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_token_to_str(void *obj, H5I_type_t obj_type,
                                          const H5O_token_t *token,
                                          char **token_str) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL TOKEN To string\n");
#endif

  /* Sanity checks */
  assert(obj);
  assert(token);
  assert(token_str);

  ret_value = H5VLtoken_to_str(o->under_object, obj_type, o->under_vol_id,
                               token, token_str);

  return ret_value;
} /* end H5VL_cache_ext_token_to_str() */

/*---------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_token_from_str
 *
 * Purpose:     Deserialize the connector's object token from a string.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5VL_cache_ext_token_from_str(void *obj, H5I_type_t obj_type,
                                            const char *token_str,
                                            H5O_token_t *token) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL TOKEN From string\n");
#endif

  /* Sanity checks */
  assert(obj);
  assert(token);
  assert(token_str);

  ret_value = H5VLtoken_from_str(o->under_object, obj_type, o->under_vol_id,
                                 token_str, token);

  return ret_value;
} /* end H5VL_cache_ext_token_from_str() */

/*-------------------------------------------------------------------------
 * Function:    H5VL_cache_ext_optional
 *
 * Purpose:     Handles the generic 'optional' callback
 *
 * Return:      SUCCEED / FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t H5VL_cache_ext_optional(void *obj, H5VL_optional_args_t *args,
                               hid_t dxpl_id, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL generic Optional\n");
#endif

  ret_value =
      H5VLoptional(o->under_object, o->under_vol_id, args, dxpl_id, req);

  return ret_value;
} /* end H5VL_cache_ext_optional() */

/*-------------------------------------------------------------------------
 * Function:    create_file_cache_on_local_storage
 *
 * Purpose:     create a file cache on the local storage
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t create_file_cache_on_local_storage(void *obj, void *file_args,
                                                 void **req) {

  file_args_t *args = (file_args_t *)file_args;
  const char *name = args->name;
  herr_t ret_value;
  hsize_t size_f;
  H5VL_cache_ext_t *file = (H5VL_cache_ext_t *)obj;

  H5VL_cache_ext_info_t *info;
  /* Get copy of our VOL info from FAPL */
  H5Pget_vol_info(args->fapl_id, (void **)&info);

  if (file->write_cache) {
    if (file->H5DWMM == NULL) {
      file->H5DWMM = (io_handler_t *)malloc(sizeof(io_handler_t));
      file->H5DWMM->mpi = (MPI_INFO *)malloc(sizeof(MPI_INFO));
      file->H5DWMM->io = (IO_THREAD *)malloc(sizeof(IO_THREAD));
      file->H5DWMM->cache = (cache_t *)malloc(sizeof(cache_t));
      file->H5DWMM->mmap = (MMAP *)malloc(sizeof(MMAP));
    } else {
#ifndef NDEBUG
      if (file->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] file_cache_create: cache data already exist. "
               "Remove first!\n");
#endif
      return SUCCEED;
    }
#ifndef NDEBUG
    if (file->H5DWMM->mpi->rank == io_node())
      LOG(-1, "create file cache on local storage\n");
#endif

    // getting mpi info
    MPI_Comm comm, comm_dup;
    MPI_Info mpi_info;
    H5Pget_fapl_mpio(args->fapl_id, &comm, &mpi_info);
    MPI_Comm_dup(comm, &file->H5DWMM->mpi->comm);
    MPI_Comm_rank(comm, &file->H5DWMM->mpi->rank);
    MPI_Comm_size(comm, &file->H5DWMM->mpi->nproc);
    MPI_Comm_split_type(file->H5DWMM->mpi->comm, MPI_COMM_TYPE_SHARED, 0,
                        MPI_INFO_NULL, &file->H5DWMM->mpi->node_comm);
    MPI_Comm_rank(file->H5DWMM->mpi->node_comm, &file->H5DWMM->mpi->local_rank);
    file->H5LS->io_node =
        (file->H5DWMM->mpi->local_rank == 0); // set up I/O node
    MPI_Comm_size(file->H5DWMM->mpi->node_comm, &file->H5DWMM->mpi->ppn);
    file->H5DWMM->io->num_request = 0;

    file->H5DWMM->cache = (cache_t *)malloc(sizeof(cache_t));
    file->H5DWMM->cache->mspace_total =
        file->H5LS->write_buffer_size * file->H5DWMM->mpi->ppn;
    if (file->H5LS->mspace_total < file->H5DWMM->cache->mspace_total) {
      if (file->H5DWMM->mpi->rank == 0)
        fprintf(
            STDERR,
            " [CACHE VOL] **WARNING: The aggregate write buffer per node is "
            "larger than the size of the cache storage. \n"
            "        Will turn off Cache effect.\n"
            "        Try to decrease HDF5_CACHE_WRITE_BUFFER_SIZE.\n");
      file->write_cache = false;
      return FAIL;
    } else if (H5LSclaim_space(file->H5LS,
                               file->H5LS->write_buffer_size *
                                   file->H5DWMM->mpi->ppn,
                               HARD, file->H5LS->replacement_policy) == FAIL) {
      printf(" [CACHE VOL] **WARNING: Unable to claim space, turning off write "
             "cache\n");
      file->write_cache = false;
      return FAIL;
    }

    file->H5DWMM->cache->mspace_left = file->H5DWMM->cache->mspace_total;
    file->H5DWMM->cache->mspace_per_rank_total =
        file->H5DWMM->cache->mspace_total / file->H5DWMM->mpi->ppn;
    file->H5DWMM->cache->mspace_per_rank_left =
        file->H5DWMM->cache->mspace_per_rank_total;
    file->H5DWMM->cache->purpose = WRITE;
    file->H5DWMM->cache->duration = PERMANENT;

    if (file->H5LS->path != NULL) {
      strcpy(file->H5DWMM->cache->path, file->H5LS->path);
      strcat(file->H5DWMM->cache->path, "/");
      strcat(file->H5DWMM->cache->path, basename((char *)name));
      strcat(file->H5DWMM->cache->path, "-cache/");
      // mkdir(file->H5DWMM->cache->path, 0755); // setup the folder with the
      // name of the file, and put everything under it.

      strcpy(file->H5DWMM->mmap->fname, file->H5DWMM->cache->path);
      strcat(file->H5DWMM->mmap->fname, "mmap-");
      char rnd[255];
      sprintf(rnd, "%d", file->H5DWMM->mpi->rank);
      strcat(file->H5DWMM->mmap->fname, rnd);
      strcat(file->H5DWMM->mmap->fname, ".dat");
#ifndef NDEBUG
      if (debug_level() > 0 && io_node() == file->H5DWMM->mpi->rank) {
        printf(" [CACHE VOL] **Using node local storage to cache the file\n");
        printf(" [CACHE VOL] **path: %s\n", file->H5DWMM->cache->path);
        printf(" [CACHE VOL] **fname: %20s\n", file->H5DWMM->mmap->fname);
      }
#endif
    }

    file->H5LS->mmap_cls->create_write_mmap(file->H5DWMM->mmap,
                                            file->H5LS->write_buffer_size);

    file->H5DWMM->io->request_list = (task_data_t *)malloc(sizeof(task_data_t));
    file->H5DWMM->io->request_list->req =
        NULL; /* Important to initialize the req pointer to be NULL */
    file->H5DWMM->io->request_list->id = 0;
    file->H5DWMM->io->current_request = file->H5DWMM->io->request_list;
    file->H5DWMM->io->first_request = file->H5DWMM->io->request_list;

    H5LSregister_cache(file->H5LS, file->H5DWMM->cache, (void *)file);

    file->H5DWMM->io->offset_current = 0;
    file->H5DWMM->mmap->offset = 0;
  }

  if (file->read_cache) {
    if (file->H5DRMM == NULL) {
      file->H5DRMM = (io_handler_t *)malloc(sizeof(io_handler_t));
      file->H5DRMM->mpi = (MPI_INFO *)malloc(sizeof(MPI_INFO));
      file->H5DRMM->io = (IO_THREAD *)malloc(sizeof(IO_THREAD));
      file->H5DRMM->cache = (cache_t *)malloc(sizeof(cache_t));
      file->H5DRMM->mmap = (MMAP *)malloc(sizeof(MMAP));
    } else {
#ifndef NDEBUG
      if (file->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] file cache create: cache data already exist. "
               "Remove first!\n");
#endif
      return SUCCEED;
    }

    MPI_Comm comm;
    MPI_Info info_mpi;
    H5Pget_fapl_mpio(args->fapl_id, &comm, &info_mpi);
    MPI_Comm_dup(comm, &file->H5DRMM->mpi->comm);
    MPI_Comm_rank(comm, &file->H5DRMM->mpi->rank);
    MPI_Comm_size(comm, &file->H5DRMM->mpi->nproc);
    MPI_Comm_split_type(file->H5DRMM->mpi->comm, MPI_COMM_TYPE_SHARED, 0,
                        MPI_INFO_NULL, &file->H5DRMM->mpi->node_comm);
    MPI_Comm_rank(file->H5DRMM->mpi->node_comm, &file->H5DRMM->mpi->local_rank);
    file->H5LS->io_node =
        (file->H5DRMM->mpi->local_rank == 0); // set io_node for H5LS;
    MPI_Comm_size(file->H5DRMM->mpi->node_comm, &file->H5DRMM->mpi->ppn);
    /* setting up cache within a folder */

    if (file->H5LS->path != NULL) {
      strcpy(file->H5DRMM->cache->path, file->H5LS->path);
      strcat(file->H5DRMM->cache->path, "/");
      strcat(file->H5DRMM->cache->path, basename((char *)name));
      strcat(file->H5DRMM->cache->path, "/");
#ifndef NDEBUG
      if (file->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] file cache created: %s\n",
               file->H5DRMM->cache->path);
#endif
    }
    // mkdir(file->H5DRMM->cache->path, 0755);
  }
  return SUCCEED;
}

static herr_t remove_file_cache_on_local_storage(void *file, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  herr_t ret_value;
  if (o->write_cache) {
    H5VL_cache_ext_file_wait(file);
    o->H5LS->mmap_cls->remove_write_mmap(o->H5DWMM->mmap, 0);
    if (H5LSremove_cache(o->H5LS, o->H5DWMM->cache) != SUCCEED) {
      printf(" [CACHE VOL] **WARNNING: could not remove cache %s\n",
             o->H5DWMM->cache->path);
      return FAIL;
    }
    /* free o->H5DWMM object. Notice that H5DWMM->cache has already been freed
     * in H5LSremove_cache */
    free(o->H5DWMM->io);
    free(o->H5DWMM->mmap);
    free(o->H5DWMM);
    o->H5DWMM = NULL;
  }
  if (o->read_cache && (!o->write_cache)) {
    if (o->H5LS->io_node)
      o->H5LS->mmap_cls->removeCacheFolder(
          o->H5DRMM->cache->path); // remove the file
    /* free o->H5DRMM object. Notice that H5DWMM->cache has already been freed
     * in H5LSremove_cache */
    free(o->H5DRMM->io);
    free(o->H5DRMM->mmap);
    free(o->H5DRMM);
    o->H5DRMM = NULL;
  }
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    create_dataset_cache_on_local_storage
 *
 * Purpose:     creating dataset cache for read purpose
 *
 * Return:      Success:    0
 *              Failure:    -1
 * Comment:
 *-------------------------------------------------------------------------
 */
static herr_t create_dataset_cache_on_local_storage(void *obj, void *dset_args,
                                                    void **req) {
  // set up read cache: obj, dset object
  // loc - where is the dataset located - group or file object
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET cache create \n");
#endif
  dset_args_t *args = (dset_args_t *)dset_args;
  const char *name = args->name;
  herr_t ret_value;
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset->parent;
  H5VL_cache_ext_t *p = o;
  while (o->parent != NULL)
    o = (H5VL_cache_ext_t *)o->parent;
  if (dset->read_cache) {
    dset->H5DRMM = (io_handler_t *)malloc(sizeof(io_handler_t));
    dset->H5DRMM->mpi = (MPI_INFO *)malloc(sizeof(MPI_INFO));
    memcpy(dset->H5DRMM->mpi, o->H5DRMM->mpi, sizeof(MPI_INFO));
    /* we do this instead of directly setting dset->..->mpi = o->...->mpi
     * because of MPI_Win for different datasets should be different */
    dset->H5DRMM->io = (IO_THREAD *)malloc(sizeof(IO_THREAD));
    dset->H5DRMM->mmap = (MMAP *)malloc(sizeof(MMAP));

    char fname[255];

    file_get_name(o->under_object, o->under_vol_id, sizeof(fname), fname,
                  H5P_DATASET_XFER_DEFAULT, NULL);
    if (o->H5DRMM == NULL || o->H5DRMM->cache == NULL) {
      file_args_t file_args;

      file_args.name = fname;
      file_args.fapl_id = file_get_fapl(o->under_object, o->under_vol_id,
                                        H5P_DATASET_XFER_DEFAULT, NULL);

      o->read_cache = true;
      o->H5LS->cache_io_cls->create_file_cache((void *)o, &file_args, req);
    }
    int np;
    MPI_Comm_rank(dset->H5DRMM->mpi->comm, &np);

    dset->H5DRMM->io->batch_cached = true;
    dset->H5DRMM->io->dset_cached = false;

    //    dataset_get_wrapper(dset->under_object, dset->under_vol_id,
    //    H5VL_DATASET_GET_TYPE, H5P_DATASET_XFER_DEFAULT, NULL,
    //    &dset->H5DRMM->dset.h5_datatype);
    dset->H5DRMM->dset.h5_datatype = H5Tcopy(args->type_id);
    dset->H5DRMM->dset.esize = H5Tget_size(args->type_id);
    // hid_t fspace;
    // dataset_get_wrapper(dset->under_object, dset->under_vol_id,
    // H5VL_DATASET_GET_SPACE, H5P_DATASET_XFER_DEFAULT, NULL, &fspace);
    int ndims = H5Sget_simple_extent_ndims(args->space_id);
    hsize_t *gdims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
    H5Sget_simple_extent_dims(args->space_id, gdims, NULL);
    hsize_t dim = 1; // compute the size of a single sample
    int i;
    for (i = 1; i < ndims; i++)
      dim = dim * gdims[i];

    dset->H5DRMM->dset.sample.nel = dim;
    dset->H5DRMM->dset.sample.dim = ndims - 1;
    dset->H5DRMM->dset.ns_glob = gdims[0];
    dset->H5DRMM->dset.ns_cached = 0;
    parallel_dist(gdims[0], dset->H5DRMM->mpi->nproc, dset->H5DRMM->mpi->rank,
                  &dset->H5DRMM->dset.ns_loc, &dset->H5DRMM->dset.s_offset);
    free(gdims);
    dset->H5DRMM->dset.sample.size =
        dset->H5DRMM->dset.esize * dset->H5DRMM->dset.sample.nel;
    dset->H5DRMM->dset.size =
        dset->H5DRMM->dset.sample.size * dset->H5DRMM->dset.ns_loc;
#ifndef NDEBUG
    LOG(dset->H5DRMM->mpi->rank, "Claim space");
#endif
    if (H5LSclaim_space(dset->H5LS,
                        dset->H5DRMM->dset.size * dset->H5DRMM->mpi->ppn, HARD,
                        dset->H5LS->replacement_policy) == SUCCEED) {
      dset->H5DRMM->cache = (cache_t *)malloc(sizeof(cache_t));

      // set cache size

      dset->H5DRMM->cache->mspace_per_rank_total = dset->H5DRMM->dset.size;
      dset->H5DRMM->cache->mspace_per_rank_left =
          dset->H5DRMM->cache->mspace_per_rank_total;

      dset->H5DRMM->cache->mspace_total =
          dset->H5DRMM->dset.size * dset->H5DRMM->mpi->ppn;
      dset->H5DRMM->cache->mspace_left = dset->H5DRMM->cache->mspace_total;

      if (dset->H5LS->path != NULL) {
        strcpy(dset->H5DRMM->cache->path, p->H5DRMM->cache->path); // create
        strcat(dset->H5DRMM->cache->path, "/");
        strcat(dset->H5DRMM->cache->path, name);
        strcat(dset->H5DRMM->cache->path, "/");
        strcpy(dset->H5DRMM->mmap->fname, dset->H5DRMM->cache->path);
        strcat(dset->H5DRMM->mmap->fname, "/dset-mmap-");
        char cc[255];
        int2char(dset->H5DRMM->mpi->rank, cc);
        strcat(dset->H5DRMM->mmap->fname, cc);
        strcat(dset->H5DRMM->mmap->fname, ".dat");
#ifndef NDEBUG
        if (dset->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
          printf(" [CACHE VOL] Dataset read cache created: %s\n",
                 dset->H5DRMM->mmap->fname);
#endif
      }

      H5LSregister_cache(dset->H5LS, dset->H5DRMM->cache, obj);

      // create mmap window
      hsize_t ss = round_page(dset->H5DRMM->dset.size);

      dset->H5LS->mmap_cls->create_read_mmap(dset->H5DRMM->mmap, ss);

      // create a new MPI data type based on the size of the element.
      MPI_Datatype type[1] = {MPI_BYTE};
      int blocklen[1] = {dset->H5DRMM->dset.esize};
      MPI_Aint disp[1] = {0};
      MPI_Type_create_struct(1, blocklen, disp, type,
                             &dset->H5DRMM->dset.mpi_datatype);
      MPI_Type_commit(&dset->H5DRMM->dset.mpi_datatype);
      // creeate MPI windows for both main threead and I/O thread.
#ifndef NDEBUG
      LOG(dset->H5DRMM->mpi->rank, "Created MMAP 0 ");
      if (dset->H5DRMM->mpi->rank == io_node() && debug_level() > 1) {
        printf("Size: %lld\n", ss);
      }
#endif
      // madvise(dset->H5DRMM->mmap->buf, ss, MADV_FREE);
      MPI_Win_create(dset->H5DRMM->mmap->buf, ss, dset->H5DRMM->dset.esize,
                     MPI_INFO_NULL, dset->H5DRMM->mpi->comm,
                     &dset->H5DRMM->mpi->win);
#ifndef NDEBUG
      LOG(dset->H5DRMM->mpi->rank, "Created MMAP 1");
#endif
      dset->read_cache_info_set = true;
    } else {
      if (dset->H5DRMM->mpi->rank == 0)
        printf(" [CACHE VOL] **WARNING: Unable to allocate space to the "
               "dataset for "
               "cache; read cache function will be turned off\n");
      dset->read_cache = false;
      free(dset->H5DRMM);
      dset->H5DRMM = NULL;
    }
  }
  if (dset->write_cache) {
    dset->H5DWMM = o->H5DWMM;
  }
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    create_dataset_cache_on_local_storage
 *
 * Purpose:     creating cache for group
 *
 * Return:      Success:    0
 *              Failure:    -1
 * Comment:
 *-------------------------------------------------------------------------
 */
static herr_t create_group_cache_on_local_storage(void *obj, void *group_args,
                                                  void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL group cache create \n");
#endif
  group_args_t *args = (group_args_t *)group_args;
  const char *name = args->name;
  herr_t ret_value;
  H5VL_cache_ext_t *group = (H5VL_cache_ext_t *)obj;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)group->parent;
  if (group->read_cache) {
    group->H5DRMM = (io_handler_t *)malloc(sizeof(io_handler_t));
    group->H5DRMM->cache = (cache_t *)malloc(sizeof(cache_t));
    group->H5DRMM->mpi = (MPI_INFO *)malloc(sizeof(MPI_INFO));
    memcpy(group->H5DRMM->mpi, o->H5DRMM->mpi, sizeof(MPI_INFO));
    if (group->H5LS->path != NULL) {
      strcpy(group->H5DRMM->cache->path, o->H5DRMM->cache->path); // create
      strcat(group->H5DRMM->cache->path, "/");
      strcat(group->H5DRMM->cache->path, name);
      strcat(group->H5DRMM->cache->path, "/");
#ifndef NDEBUG
      if (group->H5DRMM->mpi->rank == io_node() && debug_level() > 1)
        printf(" [CACHE VOL] group cache created: %s\n",
               group->H5DRMM->cache->path);
#endif
    }
  }
  if (group->write_cache) {
    group->H5DWMM = o->H5DWMM;
  }
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    create_dataset_cache_on_local_storage
 *
 * Purpose:     creating cache for group
 *
 * Return:      Success:    0
 *              Failure:    -1
 * Comment:
 *-------------------------------------------------------------------------
 */
static herr_t remove_group_cache_on_local_storage(void *obj, void **req) {
  H5VL_cache_ext_t *group = (H5VL_cache_ext_t *)obj;
  if (group->read_cache) {
    free(group->H5DRMM->cache);
    free(group->H5DRMM);
  }

  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    remove_dataset_cache_on_storage
 *
 *
 * Return:      Success:    0
 *              Failure:    -1, dataset not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t remove_dataset_cache_on_local_storage(void *dset, void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Cache remove\n");
#endif
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value = SUCCEED;
  if (o->write_cache) {
    double t0 = MPI_Wtime();
    H5VL_cache_ext_dataset_wait(dset);
    double t1 = MPI_Wtime();
#ifndef NDEBUG
    if (debug_level() > 1 && o->H5DWMM->mpi->rank == io_node())
      printf(" [CACHE VOL] dataset_wait time: %f\n", t1 - t0);
#endif
    o->H5DWMM = NULL;
  }
  if (o->read_cache) {
    hsize_t ss = round_page(o->H5DRMM->dset.size);
    o->H5LS->mmap_cls->remove_read_mmap(o->H5DRMM->mmap, ss);
    if (ss > 0)
      MPI_Win_free(&o->H5DRMM->mpi->win);
    if (H5LSremove_cache(o->H5LS, o->H5DRMM->cache) != SUCCEED) {
      printf(" [CACHE VOL] UNABLE TO REMOVE CACHE: %s\n",
             o->H5DRMM->cache->path);
    }
    free(o->H5DRMM);
    o->H5DRMM = NULL;
  }
  return ret_value;
} /* end H5VL_cache_ext_dataset_cache_remove() */

/*-------------------------------------------------------------------------
 * Function:    write_data_to_local_storage2
 *
 * Purpose:     cache function for storing read dataset to the local storage
 *
 * Return:      NULL
 *
 *-------------------------------------------------------------------------
 */
static void *write_data_to_local_storage2(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          const void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
#ifndef NDEBUG
  if (debug_level() > 1 && o->H5DRMM->mpi->rank == io_node())
    printf(" [CACHE VOL] caching data to local storage using MPI_Put\n");
#endif
  hsize_t bytes = get_buf_size(mem_space_id, mem_type_id);
  get_samples_from_filespace(file_space_id, &o->H5DRMM->dset.batch,
                             &o->H5DRMM->dset.contig_read);
  o->H5DRMM->mmap->tmp_buf = (void *)buf;
  o->H5DRMM->io->batch_cached = false;
  io_handler_t *dmm = (io_handler_t *)o->H5DRMM;
  if (!dmm->io->batch_cached) {
    char *p_mem = (char *)dmm->mmap->tmp_buf;
#ifndef NDEBUG
    if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
      printf(" [CACHE VOL] MPI_Win_fence mode_no_precede\n");
#endif

    MPI_Win_fence(MPI_MODE_NOPRECEDE, dmm->mpi->win);
    int batch_size = dmm->dset.batch.size;
    if (dmm->dset.contig_read) {
      int dest = dmm->dset.batch.list[0];
      int src = dest / dmm->dset.ns_loc;
      assert(src < dmm->mpi->nproc);
      MPI_Aint offset = (dest % dmm->dset.ns_loc) * dmm->dset.sample.nel;
#ifndef NDEBUG
      if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] MPI_put\n");
#endif

      MPI_Put(p_mem, dmm->dset.sample.nel * batch_size, dmm->dset.mpi_datatype,
              src, offset, dmm->dset.sample.nel * batch_size,
              dmm->dset.mpi_datatype, dmm->mpi->win);
      // madvise(p_mem, batch_size * dmm->dset.sample.size, MADV_FREE);
#ifndef NDEBUG
      if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
        printf(" [CACHE VOL] MPI_put done\n");
#endif
    } else {
      int i = 0;
      for (i = 0; i < batch_size; i++) {
        int dest = dmm->dset.batch.list[i];
        int src = dest / dmm->dset.ns_loc;
        assert(src < dmm->mpi->nproc);
        MPI_Aint offset = (dest % dmm->dset.ns_loc) * dmm->dset.sample.nel;
#ifndef NDEBUG
        if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
          printf(" [CACHE VOL] MPI_put\n");
#endif

        MPI_Put(&p_mem[i * dmm->dset.sample.size], dmm->dset.sample.nel,
                dmm->dset.mpi_datatype, src, offset, dmm->dset.sample.nel,
                dmm->dset.mpi_datatype, dmm->mpi->win);
        // madvise(&p_mem[i*dmm->dset.sample.size], dmm->dset.sample.size,
        // MADV_FREE);
#ifndef NDEBUG
        if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
          printf(" [CACHE VOL] MPI_put done\n");
#endif
      }
    }
    MPI_Win_fence(MPI_MODE_NOSUCCEED, dmm->mpi->win);
#ifndef NDEBUG
    if (debug_level() > 10 && o->H5DRMM->mpi->rank == io_node())
      printf(" [CACHE VOL] MPI_Win_fence mode_no_precede\n");
#endif
    H5LSrecord_cache_access(dmm->cache);
    dmm->io->batch_cached = true;
    dmm->dset.ns_cached += dmm->dset.batch.size;
    bool dset_cached;
    if (dmm->dset.ns_cached >= dmm->dset.ns_loc) {
      dmm->io->dset_cached = true;
    }
    MPI_Allreduce(&dmm->io->dset_cached, &dset_cached, 1, MPI_C_BOOL, MPI_LAND,
                  dmm->mpi->comm);
    dmm->io->dset_cached = dset_cached;
  }
  return NULL;
}

/* writing data to the local storage */
static void *write_data_to_local_storage(void *dset, hid_t mem_type_id,
                                         hid_t mem_space_id,
                                         hid_t file_space_id, hid_t plist_id,
                                         const void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  hsize_t size = get_buf_size(mem_space_id, mem_type_id);
  void *p = o->H5LS->mmap_cls->write_buffer_to_mmap(mem_space_id, mem_type_id,
                                                    buf, size, o->H5DWMM->mmap);
  return p;
}

/*-------------------------------------------------------------------------
 * Function:    read_data_from_storage
 *
 * Purpose:     Reads data elements from a dataset cache into a buffer.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t read_data_from_local_storage(void *dset, hid_t mem_type_id,
                                           hid_t mem_space_id,
                                           hid_t file_space_id, hid_t plist_id,
                                           void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value;

#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL DATASET Read from cache\n");
#endif
  bool contig = false;
  BATCH b;
  get_samples_from_filespace(file_space_id, &b, &contig);
  MPI_Win_fence(MPI_MODE_NOPUT | MPI_MODE_NOPRECEDE, o->H5DRMM->mpi->win);
  char *p_mem = (char *)buf;
  int batch_size = b.size;
  if (!contig) {
    int i = 0;
    for (i = 0; i < batch_size; i++) {
      int dest = b.list[i];
      int src = dest / o->H5DRMM->dset.ns_loc;
      MPI_Aint offset =
          (dest % o->H5DRMM->dset.ns_loc) * o->H5DRMM->dset.sample.nel;
      MPI_Get(&p_mem[i * o->H5DRMM->dset.sample.size],
              o->H5DRMM->dset.sample.nel, o->H5DRMM->dset.mpi_datatype, src,
              offset, o->H5DRMM->dset.sample.nel, o->H5DRMM->dset.mpi_datatype,
              o->H5DRMM->mpi->win);
    }
  } else {
    int dest = b.list[0];
    int src = dest / o->H5DRMM->dset.ns_loc;
    MPI_Aint offset =
        (dest % o->H5DRMM->dset.ns_loc) * o->H5DRMM->dset.sample.nel;
    MPI_Get(p_mem, o->H5DRMM->dset.sample.nel * batch_size,
            o->H5DRMM->dset.mpi_datatype, src, offset,
            o->H5DRMM->dset.sample.nel * batch_size,
            o->H5DRMM->dset.mpi_datatype, o->H5DRMM->mpi->win);
  }
  MPI_Win_fence(MPI_MODE_NOSUCCEED, o->H5DRMM->mpi->win);
  H5LSrecord_cache_access(o->H5DRMM->cache);
  ret_value = 0;
  return ret_value;
} /* end  */

#if H5_VERSION_GE(1, 13, 3)
static herr_t flush_data_from_local_storage(size_t count, void *dset[],
                                            void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL flush data from local storage\n");
#endif
  void *obj_local;
  void **obj = &obj_local;
  size_t i;
  /* Allocate obj array if necessary */
  if (count > 1)
    if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
      return -1;

  /* Build obj array */
  for (i = 0; i < count; i++) {
    /* Get the object */
    obj[i] = ((H5VL_cache_ext_t *)dset[i])->under_object;
    /* Make sure the class matches */
    if (((H5VL_cache_ext_t *)dset[i])->under_vol_id !=
        ((H5VL_cache_ext_t *)dset[0])->under_vol_id)
      return -1;
  }
  task_data_t *task =
      (task_data_t *)((H5VL_cache_ext_t *)dset[0])->H5DWMM->io->request_list;
  if (getenv("HDF5_ASYNC_DELAY_TIME")) {
    int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
    H5Pset_dxpl_delay(task->xfer_plist_id, delay_time);
  }
  task->req = NULL;
  herr_t ret_value = H5VLdataset_write(
      count, obj, ((H5VL_cache_ext_t *)dset[0])->under_vol_id,
      task->mem_type_id, task->mem_space_id, task->file_space_id,
      task->xfer_plist_id, task->buf, &task->req);
  assert(task->req != NULL);
  for (size_t i = 0; i < count; i++) {
    H5ESinsert_request(((H5VL_cache_ext_t *)dset[i])->es_id,
                       ((H5VL_cache_ext_t *)dset[i])->under_vol_id,
                       task->req); // adding this for event set
    ((H5VL_cache_ext_t *)dset[i])->num_request_dataset++;
  }
  if (getenv("HDF5_ASYNC_DELAY_TIME"))
    H5Pset_dxpl_delay(task->xfer_plist_id, 0);
  H5VL_request_status_t status;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset[0];
  // building next task
  o->H5DWMM->io->request_list->next =
      (task_data_t *)malloc(sizeof(task_data_t));
  o->H5DWMM->io->request_list->next->req = NULL;
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] Flushing I/O for task %d;\n", task->id);
#endif
  o->H5DWMM->io->request_list->next->id = o->H5DWMM->io->request_list->id + 1;
  o->H5DWMM->io->request_list = o->H5DWMM->io->request_list->next;
  // record the total number of request
  o->H5DWMM->io->num_request++;
  return ret_value;
}
#else
/*
  this is for migration data from storage to the lower layer of storage
 */
static herr_t flush_data_from_local_storage(void *dset, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  task_data_t *task = (task_data_t *)o->H5DWMM->io->request_list;
  task->req = NULL;
#ifndef NDEBUG
  if (debug_level() > 1 && io_node() == o->H5DWMM->mpi->rank)
    printf(" [CACHE VOL] flush data from local storage\n");
#endif
  if (getenv("HDF5_ASYNC_DELAY_TIME")) {
    int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
    H5Pset_dxpl_delay(task->xfer_plist_id, delay_time);
  }
  herr_t ret_value = H5VLdataset_write(
      o->under_object, o->under_vol_id, task->mem_type_id, task->mem_space_id,
      task->file_space_id, task->xfer_plist_id, task->buf, &task->req);
  assert(task->req != NULL);
  H5ESinsert_request(o->es_id, o->under_vol_id,
                     task->req); // adding this for event set
  if (getenv("HDF5_ASYNC_DELAY_TIME"))
    H5Pset_dxpl_delay(task->xfer_plist_id, 0);
  H5VL_request_status_t status;
  // building next task
  o->H5DWMM->io->request_list->next =
      (task_data_t *)malloc(sizeof(task_data_t));
  o->H5DWMM->io->request_list->next->req = NULL;
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
    printf(" [CACHE VOL] Flushing I/O for task %d;\n", task->id);
#endif
  o->H5DWMM->io->request_list->next->id = o->H5DWMM->io->request_list->id + 1;
  o->H5DWMM->io->request_list = o->H5DWMM->io->request_list->next;
  // record the total number of request
  o->H5DWMM->io->num_request++;
  o->num_request_dataset++;
  return ret_value;
}
#endif

/*-------------------------------------------------------------------------
 * Function:    create_file_cache_on_global_storage
 *
 * Purpose:     create a file cache on a global storage
 *
 * Main works:   creating a corresponding HDF5 file on the global cache storage
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t create_file_cache_on_global_storage(void *obj, void *file_args,
                                                  void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL File cache create \n");
#endif
  file_args_t *args = (file_args_t *)file_args;
  herr_t ret_value;
  hsize_t size_f;

  H5VL_cache_ext_t *file = (H5VL_cache_ext_t *)obj;
  // hid_t fapl_id = args->fapl_id;
  const char *name = args->name;

  H5VL_cache_ext_info_t *info;
  /* Get copy of our VOL info from FAPL */
  H5Pget_vol_info(args->fapl_id, (void **)&info);

  if (file->write_cache || file->read_cache) {
    if (file->H5DWMM == NULL) {
      file->H5DWMM = (io_handler_t *)malloc(sizeof(io_handler_t));
      file->H5DWMM->mpi = (MPI_INFO *)malloc(sizeof(MPI_INFO));
      file->H5DWMM->io = (IO_THREAD *)malloc(sizeof(IO_THREAD));
      file->H5DWMM->mmap = (MMAP *)malloc(sizeof(MMAP));
      file->H5DWMM->cache = (cache_t *)malloc(sizeof(cache_t));
    } else {
      if (file->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] file_cache_create: cache data already exist. "
               "Remove first!\n");
      return FAIL;
    }
    MPI_Comm comm, comm_dup;
    MPI_Info mpi_info;

    H5Pget_fapl_mpio(args->fapl_id, &comm, &mpi_info);
    MPI_Comm_dup(comm, &file->H5DWMM->mpi->comm);
    MPI_Comm_rank(comm, &file->H5DWMM->mpi->rank);
    MPI_Comm_size(comm, &file->H5DWMM->mpi->nproc);
    file->H5LS->io_node = (file->H5DWMM->mpi->rank == 0); // set up I/O node
    file->H5DWMM->io->num_request = 0;
    if (file->H5LS->path != NULL) {
      strcpy(file->H5DWMM->cache->path, file->H5LS->path);
      strcat(file->H5DWMM->cache->path, "/");
      strcat(file->H5DWMM->cache->path, basename((char *)name));
      strcat(file->H5DWMM->cache->path, "-global-cache/");
      mkdir(file->H5DWMM->cache->path,
            0755); // setup the folder with the name of the file, and put
                   // everything under it.
      strcpy(file->H5DWMM->mmap->fname, file->H5DWMM->cache->path);
      strcat(file->H5DWMM->mmap->fname, basename((char *)name));
#ifndef NDEBUG
      if (debug_level() > 0 && io_node() == file->H5DWMM->mpi->rank) {
        printf(" [CACHE VOL] **Using global storage as a cache\n");
        printf(" [CACHE VOL] **path: %s\n", file->H5DWMM->cache->path);
        printf(" [CACHE VOL] **fname: %20s\n", file->H5DWMM->mmap->fname);
      }
#endif
    }

    hid_t fapl_id_default = H5Pcopy(args->fapl_id);

    // set under vol to be native vol;

    hid_t async_vol_id = H5VLget_connector_id_by_value(H5VL_ASYNC_VALUE);
    void *p = NULL;
    native_vol_info(&p);
    H5Pset_vol(fapl_id_default, async_vol_id, p);
    free(p);

    file->hd_glob = H5Fcreate(file->H5DWMM->mmap->fname, H5F_ACC_TRUNC,
                              args->fcpl_id, fapl_id_default);
#ifndef NDEBUG
    if (debug_level() > 1 && file->H5DWMM->mpi->rank == io_node())
      printf(" [CACHE VOL] file under_vol_id: %0lx(map), %0lx\n", async_vol_id,
             file->under_vol_id);
#endif
    file->H5DWMM->io->request_list = (task_data_t *)malloc(sizeof(task_data_t));
    file->H5DWMM->io->request_list->req = NULL;
    H5LSregister_cache(file->H5LS, file->H5DWMM->cache, (void *)file);
    file->H5DWMM->io->offset_current = 0;
    file->H5DWMM->mmap->offset = 0;
    file->H5DWMM->io->request_list->id = 0;
    file->H5DWMM->io->current_request = file->H5DWMM->io->request_list;
    file->H5DWMM->io->first_request = file->H5DWMM->io->request_list;
    file->H5DRMM = file->H5DWMM;
    H5Pclose(fapl_id_default);
  }
  return SUCCEED;
}

static herr_t create_group_cache_on_global_storage(void *obj, void *group_args,
                                                   void **req) {

  group_args_t *args = (group_args_t *)group_args;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)o->parent;

  o->H5DWMM = (io_handler_t *)malloc(sizeof(io_handler_t));
  o->H5DWMM->mpi = p->H5DWMM->mpi;
  o->H5DWMM->mmap = (MMAP *)malloc(sizeof(MMAP));
  o->H5DWMM->io = p->H5DWMM->io;
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node())
    LOG(-1, "group cache create on global storage");
#endif
  o->hd_glob = H5Gcreate(p->hd_glob, args->name, args->lcpl_id, args->gcpl_id,
                         args->gapl_id);
  return SUCCEED;
}

static herr_t remove_group_cache_on_global_storage(void *obj, void **req) {
#ifndef NDEBUG
  if (RANK == io_node() && log_level() > 0)
    printf("------- EXT CACHE VOL group cache remove on global storage \n");
#endif
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)obj;
  H5Gclose(o->hd_glob);
  free(o->H5DWMM->mmap);
  free(o->H5DWMM);
  o->H5DWMM = NULL;
  return SUCCEED;
}

/*-------------------------------------------------------------------------
 * Function:    create_dataset_cache_on_local_storage
 *
 * Purpose:     creating dataset cache for read purpose
 *
 * Return:      Success:    0
 *              Failure:    -1
 * Comment:
 *-------------------------------------------------------------------------
 */
static herr_t create_dataset_cache_on_global_storage(void *obj, void *dset_args,
                                                     void **req) {
  // set up read cache: obj, dset object
  // loc - where is the dataset located - group or file object
  dset_args_t *args = (dset_args_t *)dset_args;
  herr_t ret_value;
  H5VL_cache_ext_t *dset = (H5VL_cache_ext_t *)obj;
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset->parent;
  while (o->parent != NULL)
    o = (H5VL_cache_ext_t *)o->parent;
  dset->H5LS = o->H5LS;

  if (dset->read_cache || dset->write_cache) {
    dset->H5DWMM = (io_handler_t *)malloc(sizeof(io_handler_t));
    dset->H5DWMM->mpi = o->H5DWMM->mpi;
    dset->H5DWMM->mmap = (MMAP *)malloc(sizeof(MMAP));
    dset->H5DWMM->io = o->H5DWMM->io;
    hsize_t size_f;
    char fname[255];

    file_get_name(o->under_object, o->under_vol_id, sizeof(fname), fname,
                  H5P_DATASET_XFER_DEFAULT, NULL);
    if (o->H5DWMM == NULL || o->H5DWMM->cache == NULL) {
      file_args_t file_args;
      file_args.name = fname;
      file_args.fapl_id = file_get_fapl(o->under_object, o->under_vol_id,
                                        H5P_DATASET_XFER_DEFAULT, NULL);
      o->read_cache = dset->read_cache;
      o->write_cache = dset->write_cache;
      o->H5LS->cache_io_cls->create_file_cache((void *)o, &file_args, req);
    }

    int np;
    MPI_Comm_rank(dset->H5DWMM->mpi->comm, &np);

    dset->H5DWMM->io->batch_cached = true;
    dset->H5DWMM->io->dset_cached = false;

    dset->H5DWMM->dset.esize = H5Tget_size(args->type_id);
    int ndims = H5Sget_simple_extent_ndims(args->space_id);
    hsize_t *gdims = (hsize_t *)malloc(ndims * sizeof(hsize_t));
    H5Sget_simple_extent_dims(args->space_id, gdims, NULL);
    hsize_t dim = 1; // compute the size of a single sample
    int i = 0;
    for (i = 1; i < ndims; i++)
      dim = dim * gdims[i];

    dset->H5DWMM->dset.sample.nel = dim;
    dset->H5DWMM->dset.sample.dim = ndims - 1;
    dset->H5DWMM->dset.ns_glob = gdims[0];
    dset->H5DWMM->dset.ns_cached = 0;
    parallel_dist(gdims[0], dset->H5DWMM->mpi->nproc, dset->H5DWMM->mpi->rank,
                  &dset->H5DWMM->dset.ns_loc, &dset->H5DWMM->dset.s_offset);
    free(gdims);
    dset->H5DWMM->dset.sample.size =
        dset->H5DWMM->dset.esize * dset->H5DWMM->dset.sample.nel;
    dset->H5DWMM->dset.size =
        dset->H5DWMM->dset.sample.size * dset->H5DWMM->dset.ns_loc;
#ifndef NDEBUG
    LOG(dset->H5DWMM->mpi->rank, "Claim space");
#endif
    if (H5LSclaim_space(dset->H5LS,
                        dset->H5DWMM->dset.size * dset->H5DWMM->mpi->nproc,
                        HARD, dset->H5LS->replacement_policy) == SUCCEED) {
      dset->H5DWMM->cache = (cache_t *)malloc(sizeof(cache_t));

      dset->H5DWMM->cache->mspace_per_rank_total = dset->H5DWMM->dset.size;
      dset->H5DWMM->cache->mspace_per_rank_left =
          dset->H5DWMM->cache->mspace_per_rank_total;

      dset->H5DWMM->cache->mspace_total =
          dset->H5DWMM->dset.size * dset->H5DWMM->mpi->nproc;
      dset->H5DWMM->cache->mspace_left = dset->H5DWMM->cache->mspace_total;

      if (dset->H5LS->path != NULL) {
        strcpy(dset->H5DWMM->cache->path, o->H5DWMM->cache->path); // create
        strcpy(dset->H5DWMM->mmap->fname, args->name);
#ifndef NDEBUG
        if (dset->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
          printf(" [CACHE VOL] Dataset cache created: %s\n",
                 dset->H5DWMM->mmap->fname);
#endif
      }
      // create dset on the

      H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)dset->parent;
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] Create dataset in parent group\n");
#endif
      dset->hd_glob =
          H5Dcreate(p->hd_glob, args->name, args->type_id, args->space_id,
                    args->lcpl_id, args->dcpl_id, args->dapl_id);
#ifndef NDEBUG
      if (debug_level() > 1 && dset->H5DWMM->mpi->rank == io_node())
        printf(" [CACHE VOL] Create dataset in parent group done\n");
#endif
      H5LSregister_cache(dset->H5LS, dset->H5DWMM->cache, obj);
      // create mmap window
#ifndef NDEBUG
      LOG(dset->H5DWMM->mpi->rank, " [CACHE VOL] Created dataset MAP");
#endif
      dset->read_cache_info_set = true;
      dset->H5DRMM = dset->H5DWMM;
      return SUCCEED;
    } else {
      if (dset->H5DRMM->mpi->rank == 0)
        printf(" [CACHE VOL] Unable to allocate space to the dataset for "
               "cache; read cache function will be turned off\n");
      free(dset->H5DWMM);
      dset->H5DWMM = NULL;
      dset->H5DRMM = NULL;
      return FAIL;
    }
  }

  return SUCCEED;
}

/* writing data to the global storage */
static void *write_data_to_global_storage(void *dset, hid_t mem_type_id,
                                          hid_t mem_space_id,
                                          hid_t file_space_id, hid_t plist_id,
                                          const void *buf, void **req) {
  H5VL_cache_ext_t *d = (H5VL_cache_ext_t *)dset;
  hid_t dxpl_id = H5Pcopy(plist_id);
  H5Pset_dxpl_disable_async_implicit(dxpl_id, TRUE);
  H5Dwrite_async(d->hd_glob, mem_type_id, mem_space_id, file_space_id, dxpl_id,
                 buf, H5ES_NONE);
  H5LSrecord_cache_access(d->H5DWMM->cache);
  return NULL;
}

/*-------------------------------------------------------------------------
 * Function:    read_data_from_global_storage
 *
 * Purpose:     Reads data elements from a dataset cache into a buffer.
 *
 * Return:      Success:    0
 *              Failure:    -1
 *
 *-------------------------------------------------------------------------
 */
static herr_t read_data_from_global_storage(void *dset, hid_t mem_type_id,
                                            hid_t mem_space_id,
                                            hid_t file_space_id, hid_t plist_id,
                                            void *buf, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
#ifndef NDEBUG
  if (RANK == io_node() && log_level())
    printf("------- EXT CACHE VOL DATASET Read from cache\n");
#endif
  LOG(o->H5DWMM->mpi->rank, "dataset_read_from_cache");
  hid_t dxpl_id = H5Pcopy(plist_id);
  H5Pset_dxpl_disable_async_implicit(dxpl_id, TRUE);
  H5Dread_async(o->hd_glob, mem_type_id, mem_space_id, file_space_id, plist_id,
                buf, H5ES_NONE);
  H5LSrecord_cache_access(o->H5DWMM->cache);
  return SUCCEED;
} /* end  */

/*
  this is for migration data from storage to the lower layer of storage
 */

#if H5_VERSION_GE(1, 13, 3)
static herr_t flush_data_from_global_storage(size_t count, void *dset[],
                                             void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset[0];
  task_data_t *task = (task_data_t *)o->H5DWMM->io->request_list;

  void *obj_local;
  void **obj = &obj_local;
  herr_t ret_value;
  size_t i;
  /* Allocate obj array if necessary */
  task->dataset_id = (hid_t *)malloc(count * sizeof(hid_t *));
  if (count > 1)
    if (NULL == (obj = (void **)malloc(count * sizeof(void *))))
      return -1;

  /* Build obj array */
  for (i = 0; i < count; i++) {
    /* Get the object */
    obj[i] = ((H5VL_cache_ext_t *)dset[i])->under_object;

    /* Make sure the class matches */
    if (((H5VL_cache_ext_t *)dset[i])->under_vol_id !=
        ((H5VL_cache_ext_t *)dset[0])->under_vol_id)
      return -1;
    task->dataset_id[i] = ((H5VL_cache_ext_t *)dset[i])->hd_glob;
  }

  // to call read_data_from_global_storage to get the buffer

  // question: How to combine these two calls and make them dependent from each
  // other
  hsize_t bytes;
  for (size_t i = 0; i < count; i++) {
    bytes = get_buf_size(task->mem_space_id[i], task->mem_type_id[i]);
    task->buf[i] = malloc(bytes);
  }

  task->req = NULL;
  void *req2 = NULL;
  hid_t dxpl_id = H5Pcopy(task->xfer_plist_id);
  if (getenv("HDF5_ASYNC_DELAY_TIME")) {
    int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
    // H5Pset_dxpl_delay(task->xfer_plist_id, delay_time);
    H5Pset_dxpl_delay(dxpl_id, delay_time);
  }
  // H5VL_async_pause();
  H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)o->parent;
  while (p->parent != NULL)
    p = (H5VL_cache_ext_t *)p->parent;
  if (!p->async_pause) {
    H5Pset_dxpl_pause(dxpl_id, true);
  }
  // temparally fix
  ret_value = H5VLdataset_write(count, obj, o->under_vol_id, task->mem_type_id,
                                task->mem_space_id, task->file_space_id,
                                dxpl_id, task->buf, &task->req);
  assert(task->req != NULL);

  H5Dread_multi_async(task->count, task->dataset_id, task->mem_type_id,
                      task->mem_space_id, task->file_space_id, dxpl_id,
                      task->buf, o->es_id);
  ret_value = H5ESget_requests(o->es_id, H5_ITER_DEC, NULL, &req2, 1, NULL);
  assert(req2 != NULL);

  /* Below is to make sure that the data migration will be executed one at a
   * time to prevent memory blow up */
  void *previous_req = NULL;
  if (o->H5LS->previous_write_req != NULL) {
    previous_req = o->H5LS->previous_write_req;
#ifndef NDEBUG
    if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] Adding dependency to previous write request\n");
#endif
    H5VL_async_set_request_dep(req2, previous_req);
  }
  H5VL_async_set_request_dep(task->req, req2);
  for (size_t i = 0; i < count; i++) {
    H5ESinsert_request(((H5VL_cache_ext_t *)dset[i])->es_id, o->under_vol_id,
                       task->req);
    ((H5VL_cache_ext_t *)dset[i])->num_request_dataset++;
  }
  if (!p->async_pause) {
    H5async_start(req2);
    H5async_start(task->req);
  }
  // H5VL_async_start();
  if (getenv("HDF5_ASYNC_DELAY_TIME"))
    H5Pset_dxpl_delay(dxpl_id, 0);
  H5VL_request_status_t status;
  o->H5LS->previous_write_req = task->req;
  // building next task
  o->H5DWMM->io->request_list->next =
      (task_data_t *)malloc(sizeof(task_data_t));
  o->H5DWMM->io->request_list->next->req = NULL;
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 0)
    printf(" [CACHE VOL] added task %d to the list;\n", task->id);
#endif
  o->H5DWMM->io->request_list->next->id = o->H5DWMM->io->request_list->id + 1;
  o->H5DWMM->io->request_list = o->H5DWMM->io->request_list->next;

  // record the total number of request
  o->H5DWMM->io->num_request++;
  return ret_value;
}
#else
static herr_t flush_data_from_global_storage(void *dset, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  task_data_t *task = (task_data_t *)o->H5DWMM->io->request_list;
  // to call read_data_from_global_storage to get the buffer

  // question: How to combine these two calls and make them dependent from each
  // other
  hsize_t bytes = get_buf_size(task->mem_space_id, task->mem_type_id);
  task->buf = malloc(bytes);
  task->req = NULL;
  void *req2 = NULL;
  herr_t ret_value = SUCCEED;
  hid_t dxpl_id = H5Pcopy(task->xfer_plist_id);
  if (getenv("HDF5_ASYNC_DELAY_TIME")) {
    int delay_time = atof(getenv("HDF5_ASYNC_DELAY_TIME"));
    // H5Pset_dxpl_delay(task->xfer_plist_id, delay_time);
    H5Pset_dxpl_delay(dxpl_id, delay_time);
  }
  // H5VL_async_pause();
  H5VL_cache_ext_t *p = (H5VL_cache_ext_t *)o->parent;
  while (p->parent != NULL)
    p = (H5VL_cache_ext_t *)p->parent;
  if (!p->async_pause) {
    H5Pset_dxpl_pause(dxpl_id, true);
  }

  ret_value = H5VLdataset_write(
      o->under_object, o->under_vol_id, task->mem_type_id, task->mem_space_id,
      task->file_space_id, dxpl_id, task->buf, &task->req);
  assert(task->req != NULL);

  H5Dread_async(o->hd_glob, task->mem_type_id, task->mem_space_id,
                task->file_space_id, dxpl_id, task->buf, o->es_id);
  ret_value = H5ESget_requests(o->es_id, H5_ITER_DEC, NULL, &req2, 1, NULL);
  assert(req2 != NULL);

  /* Below is to make sure that the data migration will be executed one at a
   * time to prevent memory blow up */
  void *previous_req = NULL;
  if (o->H5LS->previous_write_req != NULL) {
    previous_req = o->H5LS->previous_write_req;
#ifndef NDEBUG
    if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 1)
      printf(" [CACHE VOL] Adding dependency to previous write request\n");
#endif
    H5VL_async_set_request_dep(req2, previous_req);
  }
  H5VL_async_set_request_dep(task->req, req2);
  H5ESinsert_request(o->es_id, o->under_vol_id, task->req);
  if (!p->async_pause) {
    H5async_start(req2);
    H5async_start(task->req);
  }
  // H5VL_async_start();
  if (getenv("HDF5_ASYNC_DELAY_TIME"))
    H5Pset_dxpl_delay(dxpl_id, 0);
  H5VL_request_status_t status;
  o->H5LS->previous_write_req = task->req;
  // building next task
  o->H5DWMM->io->request_list->next =
      (task_data_t *)malloc(sizeof(task_data_t));
  o->H5DWMM->io->request_list->next->req = NULL;
#ifndef NDEBUG
  if (o->H5DWMM->mpi->rank == io_node() && debug_level() > 0)
    printf(" [CACHE VOL] added task %d to the list;\n", task->id);
#endif
  o->H5DWMM->io->request_list->next->id = o->H5DWMM->io->request_list->id + 1;
  o->H5DWMM->io->request_list = o->H5DWMM->io->request_list->next;

  // record the total number of request
  o->H5DWMM->io->num_request++;
  o->num_request_dataset++;
  return ret_value;
}
#endif
/*-------------------------------------------------------------------------
 * Function:    remove_dataset_cache_on_storage
 *
 *
 * Return:      Success:    0
 *              Failure:    -1, dataset not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t remove_dataset_cache_on_global_storage(void *dset, void **req) {
#ifndef NDEBUG
  if (RANK == io_node())
    printf("------- EXT CACHE VOL DATASET Cache remove\n");
#endif
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)dset;
  herr_t ret_value = SUCCEED;
  if (o->write_cache)
    H5VL_cache_ext_dataset_wait(dset);
  if (o->write_cache || o->read_cache) {
    H5Dclose(o->hd_glob);
    free(o->H5DWMM->cache);
    free(o->H5DWMM->mmap);
    free(o->H5DWMM);
    o->H5DWMM = NULL;
  }
  return ret_value;
} /* */

static herr_t remove_file_cache_on_global_storage(void *file, void **req) {
  H5VL_cache_ext_t *o = (H5VL_cache_ext_t *)file;
  herr_t ret_value;
  if (o->write_cache) {
    H5VL_cache_ext_file_wait(file);
    H5Fclose(o->hd_glob);
    MPI_Barrier(o->H5DWMM->mpi->comm);
    if (o->H5DWMM->mpi->rank == io_node())
      rmdirRecursive(o->H5DWMM->cache->path);
    MPI_Barrier(o->H5DWMM->mpi->comm);
    if (H5LSremove_cache(o->H5LS, o->H5DWMM->cache) != SUCCEED) {
      printf(" [CACHE VOL] Could not remove cache %s\n",
             o->H5DWMM->cache->path);
      return FAIL;
    }
    /* freeing objects. Notice that H5DWMM->cache was already freed in
     * H5LSremove_cache */
    free(o->H5DWMM->mpi);
    free(o->H5DWMM->mmap);
    free(o->H5DWMM->io);
    free(o->H5DWMM);
    o->H5DWMM = NULL;
  }
  if (o->read_cache && (!o->write_cache)) {
    H5Fclose(o->hd_glob);
    if (o->H5DWMM->mpi->rank == io_node())
      rmdirRecursive(o->H5DWMM->cache->path);
    /* freeing objects. Notice that H5DWMM->cache was already freed in
     * H5LSremove_cache */
    free(o->H5DWMM->mpi);
    free(o->H5DWMM->io);
    free(o->H5DWMM);
    o->H5DWMM = NULL;
    o->H5DRMM = NULL;
  }
  return SUCCEED;
}
