#include "stream.hpp"
#include <fibre/cpp_utils.hpp>
namespace rehab { namespace platform {
void Stream::start() {active_=true;poll();}
void Stream::stop() {
    active_=false;
    // Keep buffers alive until the transport retires pending transfers. A
    // successful late completion still belongs to the disconnected stream.
    if(reading_)discard_read_=true;
    if(writing_)discard_write_=true;
    else tx_size_=tx_offset_=0;
    disconnected(port_);
}
void Stream::poll() {
    if(!active_)return;
    if(!reading_) {
        reading_=true;fibre::TransferHandle handle;
        source_.start_read(rx_,&handle,MEMBER_CB(this,read_done));
    }
    if(!active_)return; // A synchronous read error may have stopped the stream.
    if(!writing_) {
        // The legacy STM32 USB sink requires strictly less than its 64-byte
        // endpoint size. Sharing a 63-byte MTU with UART keeps both paths valid.
        if(tx_offset_==tx_size_) {tx_size_=transmit_bytes(port_,tx_,63);tx_offset_=0;}
        if(tx_offset_<tx_size_) {
            writing_=true;fibre::TransferHandle handle;
            sink_.start_write({tx_+tx_offset_,tx_+tx_size_},&handle,MEMBER_CB(this,write_done));
        }
    }
}
void Stream::read_done(fibre::ReadResult result) {
    reading_=false;
    if(discard_read_) {discard_read_=false;if(active_)poll();return;}
    if(result.status!=fibre::kStreamOk) {stop();return;}
    if(active_ && result.end>=rx_ && result.end<=rx_+sizeof(rx_))receive_bytes(port_,rx_,size_t(result.end-rx_));
    // Rearming a successful read is safe here: native HAL streams only complete
    // data reads in their event loop. Errors are never retried recursively.
    if(active_)poll();
}
void Stream::write_done(fibre::WriteResult result) {
    writing_=false;
    if(discard_write_) {
        discard_write_=false;tx_size_=tx_offset_=0;
        if(active_)poll();
        return;
    }
    if(result.status!=fibre::kStreamOk || result.end<tx_+tx_offset_ || result.end>tx_+tx_size_) {
        tx_size_=tx_offset_=0;stop();return;
    }
    const size_t offset=size_t(result.end-tx_);
    if(offset==tx_offset_) {stop();return;}
    tx_offset_=offset;
    if(active_)poll();
}
}}
