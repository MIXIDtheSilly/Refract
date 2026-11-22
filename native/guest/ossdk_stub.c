#include <stdint.h>

typedef uintptr_t (*TelemetryMethod)(void*, ...);
typedef struct {
    const TelemetryMethod* vtable;
} TelemetryObject;

static uintptr_t inert_method(void* self, ...) {
    (void)self;
    return 0;
}

static const TelemetryMethod g_event_vtable[32] = {
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
};

static TelemetryObject g_event = {g_event_vtable};

static uintptr_t create_event(void* self, ...) {
    (void)self;
    return (uintptr_t)&g_event;
}

// Keep telemetry callers' vtable valid while dropping all collected events.
static const TelemetryMethod g_handler_vtable[32] = {
    inert_method, inert_method, inert_method, create_event,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
    inert_method, inert_method, inert_method, inert_method,
};

static TelemetryObject g_handler = {g_handler_vtable};

__attribute__((visibility("default")))
void* createTelemetryHandler(int64_t version) {
    return version == 3 ? &g_handler : 0;
}

__attribute__((visibility("default")))
void destroyTelemetryHandler(int64_t version, void* handler) {
    (void)version;
    (void)handler;
}

__attribute__((visibility("default")))
void destroyTelemetryEvent(int64_t version, void* event) {
    (void)version;
    (void)event;
}

__attribute__((visibility("default")))
void* createVrDeviceManager(int64_t version) {
    (void)version;
    return 0;
}

__attribute__((visibility("default")))
void destroyVrDeviceManager(int64_t version, void* manager) {
    (void)version;
    (void)manager;
}
