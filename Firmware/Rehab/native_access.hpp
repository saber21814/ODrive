#pragma once
#include <cstddef>
#include <cstdint>

namespace rehab {
// The implementation only masks interrupts for counter/state updates, never for
// the lifetime of this guard. A handoff waits for all admitted writes to finish.
#if defined(REHAB_V12)
bool begin_native_write();
void end_native_write();
bool native_stop_property(const void* object, const void* value, size_t length);
void native_emergency_stop();
bool native_input_allowed();
#else
inline bool begin_native_write() { return true; }
inline void end_native_write() {}
inline bool native_stop_property(const void*, const void*, size_t) { return false; }
inline void native_emergency_stop() {}
inline bool native_input_allowed() { return true; }
#endif
class NativeWriteGuard {
public:
    explicit NativeWriteGuard(bool needed = true): needed_(needed), admitted_(!needed || begin_native_write()) {}
    ~NativeWriteGuard() { if (needed_ && admitted_) end_native_write(); }
    explicit operator bool() const { return admitted_; }
    NativeWriteGuard(const NativeWriteGuard&) = delete;
    NativeWriteGuard& operator=(const NativeWriteGuard&) = delete;
private:
    bool needed_, admitted_;
};
}
