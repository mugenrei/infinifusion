#ifndef INFINIFUSION_API_H
#define INFINIFUSION_API_H

#include <stdint.h>

#if defined(_WIN32)
#define IF_CALL __cdecl
#else
#define IF_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define IF_API_VERSION 0x0001000Bu
#define IF_MAX_TEXT_BYTES 4096u
#define IF_OK 0u
#define IF_INVALID_ARGUMENT 1u
#define IF_QUEUE_FULL 18u
#define IF_INVALID_HANDLE 30u
#define IF_ABI_MISMATCH 31u
#define IF_NOT_READY 32u
#define IF_STATUS_SUMMARY UINT32_MAX
enum IF_Error {
    IF_DUPLICATE_OWNER = 2, IF_STALE_TOKEN, IF_UNKNOWN_OWNER, IF_STALE_REVISION,
    IF_REVISION_CONFLICT, IF_TYPE_MISMATCH, IF_OUT_OF_RANGE, IF_NOT_FINITE,
    IF_UNKNOWN_ENUM_SYMBOL, IF_PATH_NOT_FOUND, IF_AMBIGUOUS_PATH, IF_GUARD_MISMATCH,
    IF_CAPABILITY_UNAVAILABLE, IF_EXPECTATION_FAILED, IF_LIFETIME_UNVERIFIABLE,
    IF_LIMIT_EXCEEDED, IF_WRITE_FAILED = 19, IF_RESTORE_ERROR, IF_READ_FAILED,
    IF_TARGET_EXPIRED_ERROR, IF_GROUP_INVALID, IF_GROUP_TOO_LARGE, IF_FAULT_ERROR,
    IF_CANCELLED_ERROR, IF_NOT_FOUND, IF_PROVIDER_UNKNOWN, IF_EXTERNAL_CHANGE,
    IF_INTERNAL = 33
};
enum IF_IdentityTracking { IF_OBSERVED_ONLY, IF_OBJECT_SERIAL, IF_STRUCTURAL_EPOCH };
typedef uint64_t IF_Owner;
typedef uint64_t IF_Request;
typedef uint64_t IF_Property;
typedef struct IF_String { const char* data; uint32_t length; } IF_String;

enum IF_ValueKind { IF_BOOL, IF_INT64, IF_UINT64, IF_FLOAT64, IF_STRING, IF_ENUM, IF_BYTES = 7 };
enum IF_OperationState {
    IF_ACCEPTED, IF_WAITING_RUNTIME, IF_WAITING_TARGET, IF_PREPARED, IF_APPLIED,
    IF_ALREADY_EFFECTIVE, IF_SHADOWED, IF_REJECTED, IF_DRIFTED, IF_REMOVING,
    IF_REMOVED, IF_RESTORE_FAILED, IF_TARGET_EXPIRED, IF_CANCELLED, IF_FAULTED,
    IF_OUTCOME_UNKNOWN,
    IF_COMPLETED
};

enum IF_AssignmentFlags { IF_PERSISTENT_OVERLAY = 1u, IF_NO_RESTORE = 2u };

enum IF_TbcrMaterializationState {
    IF_TBCR_WAITING = 0, IF_TBCR_APPLIED = 1, IF_TBCR_REJECTED = 2,
    IF_TBCR_OUTCOME_UNKNOWN = 3
};
typedef struct IF_TbcrState {
    uint32_t size, state, error, reserved;
    uint64_t application;
    uint32_t update_records, append_records, before_bytes, after_bytes;
    char message[192];
} IF_TbcrState;

enum IF_TableMaterializationState {
    IF_TABLE_WAITING = 0, IF_TABLE_APPLIED = 1, IF_TABLE_REJECTED = 2,
    IF_TABLE_OUTCOME_UNKNOWN = 3
};
typedef struct IF_TableState {
    uint32_t size, state, error, reserved;
    uint64_t application;
    uint32_t updated_rows, added_rows, removed_rows, imported_fields;
    char message[192];
} IF_TableState;

typedef struct IF_Bytes { const uint8_t* data; uint32_t length; } IF_Bytes;

typedef struct IF_Value {
    uint32_t size;
    uint32_t kind;
    int64_t signed_value;
    uint64_t unsigned_value;
    double float_value;
    IF_String text;
    IF_String enum_type;
    IF_Bytes bytes;
} IF_Value;
typedef struct IF_Target {
    IF_String provider;

    IF_String object_path;
    IF_String row;
    IF_String class_guard;
} IF_Target;
enum IF_ObjectQueryFlags { IF_OBJECT_QUERY_INCLUDE_DEFAULTS = 1u };
typedef struct IF_ObjectQuery {
    uint32_t size, flags;
    uint32_t start_index, scan_limit;
    uint32_t result_limit;
    IF_String path_contains;
    IF_String class_path;
} IF_ObjectQuery;
enum IF_InvokeFlags { IF_INVOKE_EXPECT_IDENTITY = 1u };
typedef struct IF_InvokeSpec {
    uint32_t size, flags;
    IF_Target target;
    IF_String function;
    IF_String arguments;
    uint64_t expected_object_index, expected_object_serial;
    IF_String expected_properties;

} IF_InvokeSpec;
typedef struct IF_Assignment {
    uint32_t size;
    uint32_t flags;
    IF_Target target;
    IF_String property;
    IF_Value value;
    uint32_t has_expect;
    uint32_t reserved;
    IF_Value expect;
} IF_Assignment;

typedef struct IF_TbcrWrite {
    uint32_t size;
    uint32_t offset;
    IF_String property;
    IF_String type;
    uint32_t value_count;
    uint32_t reserved;
    const IF_Value* values;
} IF_TbcrWrite;
typedef struct IF_TbcrAppend {
    uint32_t size;
    uint32_t table_index;
    IF_String asset;
    IF_String table_alias;
    uint32_t record_size;
    uint32_t write_count;
    const IF_TbcrWrite* writes;
    IF_String id;
} IF_TbcrAppend;

typedef struct IF_TbcrUpdate {
    uint32_t size;
    uint32_t table_index;
    IF_String asset;
    IF_String table_alias;
    uint32_t record_size;
    uint32_t record_index;
    const IF_TbcrWrite* write;
} IF_TbcrUpdate;

typedef struct IF_TbcrCompiled {
    uint32_t size;
    uint32_t reserved;
    char provider[64];
    char asset[512];
    char row[512];
    char property[64];
    IF_Value value;
    char message[192];
    uint32_t byte_offset;
    uint32_t byte_count;
    uint8_t bytes[4096];
} IF_TbcrCompiled;
typedef struct IF_OwnerInfo {
    uint32_t size;
    int32_t priority;
    IF_String id;
    IF_String name;
    IF_String ordering_key;
} IF_OwnerInfo;
typedef struct IF_PatchSet {
    uint32_t size;
    uint32_t assignment_count;
    IF_String id;
    uint64_t revision;
    int64_t order;
    const IF_Assignment* assignments;
} IF_PatchSet;

typedef struct IF_CatalogEntry {
    uint32_t size;
    uint32_t reserved;
    IF_OwnerInfo owner;
    IF_PatchSet patch;
} IF_CatalogEntry;
typedef struct IF_Catalog {
    uint32_t size;
    uint32_t entry_count;
    uint64_t revision;
    const IF_CatalogEntry* entries;
} IF_Catalog;

typedef struct IF_Status {
    uint32_t size;
    uint32_t state;
    uint32_t error;
    uint32_t observed;
    uint32_t settled;
    uint32_t operation_count;
    uint32_t pending_count;
    uint32_t rejected_count;
    uint64_t revision;
    IF_Owner effective_owner;
    uint64_t object_index;
    uint64_t object_serial;
    uint32_t baseline_restore_skipped;
    uint32_t drift_detected;
    char message[192];
    char canonical_path[256];
} IF_Status;
typedef struct IF_ReadResult {
    uint32_t size;
    uint32_t kind;
    int64_t signed_value;
    uint64_t unsigned_value;
    double float_value;
    uint64_t object_index;
    uint64_t object_serial;
    uint32_t identity_tracking;
    uint32_t restorable;
    char text[IF_MAX_TEXT_BYTES + 1];
    char canonical_path[256];
    uint32_t byte_count;
    uint8_t bytes[4096];
} IF_ReadResult;

typedef struct IF_PropertySpec {
    uint32_t size;
    uint32_t kind;
    IF_Target target;
    IF_String property;
} IF_PropertySpec;
typedef struct IF_PropertyState {
    uint32_t size;
    uint32_t read_state;
    uint32_t read_error;
    uint32_t has_initial;
    uint32_t has_current;
    uint32_t has_intent;
    uint32_t removing;
    uint32_t reserved;
    uint64_t revision;
    uint64_t read_generation;
    IF_ReadResult initial;
    IF_ReadResult current;
    IF_Status edit;
    IF_Status acknowledgement;
} IF_PropertyState;
typedef struct IF_Capability {
    char id[64];
    uint32_t major;
    uint32_t minor;
    uint32_t available;
    uint32_t reserved;
} IF_Capability;

typedef struct IF_Metrics {
    uint32_t size;
    uint32_t faulted;
    uint64_t callback_count;
    uint64_t last_callback_us;
    uint64_t max_callback_us;
    uint64_t deadline_overruns;
    uint64_t queue_depth;
    uint64_t retained_results;
    uint64_t active_operations;
    uint64_t journal_bytes;
    uint64_t provider_calls;
    uint64_t dropped_logs;
    uint64_t property_bindings;
} IF_Metrics;

typedef struct IF_Api {
    uint32_t size;
    uint32_t version;
    uint32_t (IF_CALL *register_owner)(const IF_OwnerInfo*, IF_Owner*);
    uint32_t (IF_CALL *submit)(IF_Owner, const IF_PatchSet*, IF_Request*);
    uint32_t (IF_CALL *read)(IF_Owner, const IF_Target*, IF_String, IF_Request*);
    uint32_t (IF_CALL *remove_set)(IF_Owner, IF_String, IF_Request*);
    uint32_t (IF_CALL *disable_owner)(IF_Owner, IF_Request*);
    uint32_t (IF_CALL *status)(IF_Owner, IF_Request, uint32_t, IF_Status*);
    uint32_t (IF_CALL *copy_read)(IF_Owner, IF_Request, IF_ReadResult*);
    uint32_t (IF_CALL *release)(IF_Owner, IF_Request);
    uint32_t (IF_CALL *cancel)(IF_Owner, IF_Request);
    uint32_t (IF_CALL *capabilities)(IF_Capability*, uint32_t, uint32_t*);
    void (IF_CALL *log)(uint32_t, IF_String);
    uint32_t (IF_CALL *resume)(IF_Owner, IF_String, uint64_t revision,
                              uint32_t operation_index, const IF_Value*, IF_Request*);
    uint32_t (IF_CALL *metrics)(IF_Metrics*);
    uint32_t (IF_CALL *property_open)(IF_Owner, const IF_PropertySpec*, IF_Property*);
    uint32_t (IF_CALL *property_get)(IF_Owner, IF_Property, IF_PropertyState*);
    uint32_t (IF_CALL *property_refresh)(IF_Owner, IF_Property);

    uint32_t (IF_CALL *property_set)(IF_Owner, IF_Property, const IF_Value*, uint32_t flags, uint64_t revision);

    uint32_t (IF_CALL *property_reset)(IF_Owner, IF_Property, uint32_t flags, uint64_t revision);
    uint32_t (IF_CALL *property_remove)(IF_Owner, IF_Property);

    uint32_t (IF_CALL *property_close)(IF_Owner, IF_Property);
    uint32_t (IF_CALL *property_resume)(IF_Owner, IF_Property, const IF_Value* observed);

    uint32_t (IF_CALL *catalog_publish)(IF_Owner, const IF_Catalog*, IF_Request*);

    uint32_t (IF_CALL *tbcr_state)(IF_String asset, IF_TbcrState*);

    uint32_t (IF_CALL *tbcr_compile_append)(const IF_TbcrAppend*, IF_TbcrCompiled*);
    uint32_t (IF_CALL *tbcr_compile_update)(const IF_TbcrUpdate*, IF_TbcrCompiled*);
    uint32_t (IF_CALL *table_state)(IF_String asset, IF_TableState*);

    uint32_t (IF_CALL *object_search)(IF_Owner, const IF_ObjectQuery*, IF_Request*);
    uint32_t (IF_CALL *invoke)(IF_Owner, const IF_InvokeSpec*, IF_Request*);
} IF_Api;

typedef struct IF_Plugin {
    uint32_t size;
    uint32_t required_api_version;
    IF_OwnerInfo owner;
    uint32_t (IF_CALL *initialize)(const IF_Api*, IF_Owner);
    void (IF_CALL *poll)(const IF_Api*, IF_Owner);
} IF_Plugin;
typedef uint32_t (IF_CALL *IF_PluginEntry)(const IF_Api*, IF_Plugin*);
uint32_t IF_CALL Infinifusion_QueryApi(uint32_t requested_version,
                                     uint32_t output_size, IF_Api* output);

#ifdef __cplusplus
}
#endif
#endif
