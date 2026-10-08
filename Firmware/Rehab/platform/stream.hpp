#pragma once
#include <fibre/async_stream.hpp>
#include "runtime.hpp"
namespace rehab { namespace platform {
// Owned exclusively by the native USB/UART event-loop task. Static buffers
// remain alive through asynchronous HAL completion and disconnect callbacks.
class Stream {
public:
    Stream(unsigned port,fibre::AsyncStreamSource& source,fibre::AsyncStreamSink& sink)
        :port_(port),source_(source),sink_(sink) {}
    void start();
    void stop();
    void poll();
    bool active() const { return active_; }
private:
    void read_done(fibre::ReadResult);
    void write_done(fibre::WriteResult);
    unsigned port_;
    fibre::AsyncStreamSource& source_;fibre::AsyncStreamSink& sink_;
    uint8_t rx_[64]{},tx_[64]{};
    size_t tx_size_=0,tx_offset_=0;
    bool active_=false,reading_=false,writing_=false;
    bool discard_read_=false,discard_write_=false;
};
}}
