// Host contract tests for the actual Stream adapter and actual Fibre async
// interfaces. Only the byte-queue platform functions and endpoints are mocks.
#include "Rehab/platform/stream.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::array<std::vector<uint8_t>,3> outbound, inbound;
std::array<unsigned,3> disconnects{};
unsigned checks=0,failures=0;
struct TestFailure {};
void check(bool ok,const char* expression,int line) {
    ++checks;
    if(!ok) {++failures;std::fprintf(stderr,"FAIL stream_tests.cpp:%d: %s\n",line,expression);throw TestFailure{};}
}
#define CHECK(expression) check(bool(expression),#expression,__LINE__)
void reset_queues() {
    for(auto& q:outbound)q.clear();
    for(auto& q:inbound)q.clear();
    disconnects.fill(0);
}
std::vector<uint8_t> sequence(size_t length,uint8_t start=0) {
    std::vector<uint8_t> bytes(length);
    for(size_t i=0;i<length;++i)bytes[i]=uint8_t(start+i);
    return bytes;
}

class Source final : public fibre::AsyncStreamSource {
public:
    void start_read(fibre::bufptr_t bytes,fibre::TransferHandle* handle,
                    fibre::Callback<void,fibre::ReadResult> callback) override {
        ++starts; ++depth; maximum_depth=std::max(depth,maximum_depth);
        CHECK(depth<8 && !pending);
        if(handle)*handle=1;
        if(synchronous_error)callback.invoke({fibre::kStreamError,bytes.begin()});
        else {buffer=bytes;pending=callback;}
        --depth;
    }
    void cancel_read(fibre::TransferHandle) override {++cancels;}
    void finish(const std::vector<uint8_t>& bytes={},fibre::StreamStatus status=fibre::kStreamOk) {
        CHECK(bool(pending) && bytes.size()<=buffer.size());
        if(!bytes.empty())std::memcpy(buffer.begin(),bytes.data(),bytes.size());
        auto callback=pending; pending=nullptr;
        callback.invoke({status,buffer.begin()+bytes.size()});
    }
    fibre::bufptr_t buffer{nullptr,nullptr};
    fibre::Callback<void,fibre::ReadResult> pending;
    bool synchronous_error=false;
    unsigned starts=0,cancels=0,depth=0,maximum_depth=0;
};
class Sink final : public fibre::AsyncStreamSink {
public:
    void start_write(fibre::cbufptr_t bytes,fibre::TransferHandle* handle,
                     fibre::Callback<void,fibre::WriteResult> callback) override {
        ++starts; ++depth;maximum_depth=std::max(depth,maximum_depth);
        CHECK(depth<8 && !pending);
        submitted_sizes.push_back(bytes.size());
        if(handle)*handle=2;
        // Match the real STM32 USB sink's strict <64-byte contract.
        if(synchronous_error || bytes.size()>=64)callback.invoke({fibre::kStreamError,bytes.begin()});
        else {buffer=bytes;pending=callback;}
        --depth;
    }
    void cancel_write(fibre::TransferHandle) override {++cancels;}
    void finish(size_t accepted,fibre::StreamStatus status=fibre::kStreamOk) {
        CHECK(bool(pending) && accepted<=buffer.size());
        if(status==fibre::kStreamOk)wire.insert(wire.end(),buffer.begin(),buffer.begin()+accepted);
        const uint8_t* end=buffer.begin()+accepted;
        auto callback=pending;pending=nullptr;
        callback.invoke({status,end});
    }
    void finish_all() {finish(buffer.size());}
    fibre::cbufptr_t buffer{nullptr,nullptr};
    fibre::Callback<void,fibre::WriteResult> pending;
    std::vector<size_t> submitted_sizes;
    std::vector<uint8_t> wire;
    bool synchronous_error=false;
    unsigned starts=0,cancels=0,depth=0,maximum_depth=0;
};
void drain(Sink& sink) {
    unsigned budget=64;
    while(sink.pending) {CHECK(budget--!=0);sink.finish_all();}
}
}

namespace rehab { namespace platform {
void receive_bytes(unsigned port,const uint8_t* bytes,size_t count) {
    CHECK(port<3);inbound[port].insert(inbound[port].end(),bytes,bytes+count);
}
size_t transmit_bytes(unsigned port,uint8_t* bytes,size_t capacity) {
    CHECK(port<3);const auto count=std::min(capacity,outbound[port].size());
    std::copy_n(outbound[port].begin(),count,bytes);
    outbound[port].erase(outbound[port].begin(),outbound[port].begin()+count);
    return count;
}
void disconnected(unsigned port) {CHECK(port<3);++disconnects[port];outbound[port].clear();}
} }

namespace {
void test_start_and_wake() {
    reset_queues();Source source;Sink sink;rehab::platform::Stream stream{0,source,sink};
    stream.poll();CHECK(source.starts==0 && sink.starts==0);
    stream.start();CHECK(source.starts==1 && sink.starts==0);
    stream.poll();CHECK(source.starts==1 && sink.starts==0); // one outstanding RX
    outbound[0]=sequence(21,80);stream.poll();CHECK(sink.starts==1);
    drain(sink);CHECK(sink.wire==sequence(21,80));
    source.finish(sequence(17,33));CHECK(inbound[0]==sequence(17,33) && source.starts==2);
}
void test_usb_chunk_boundaries() {
    for(size_t length:{size_t(1),size_t(63),size_t(64),size_t(126),size_t(276)}) {
        reset_queues();Source source;Sink sink;rehab::platform::Stream stream{0,source,sink};
        const auto expected=sequence(length,13);outbound[0]=expected;stream.start();drain(sink);
        CHECK(sink.wire==expected && !disconnects[0]);
        CHECK(sink.submitted_sizes.size()==(length+62)/63);
        for(size_t submitted:sink.submitted_sizes)CHECK(submitted>0 && submitted<=63);
    }
}
void test_partial_completion() {
    reset_queues();Source source;Sink sink;rehab::platform::Stream stream{1,source,sink};
    const auto expected=sequence(139,41);outbound[1]=expected;stream.start();
    CHECK(sink.buffer.size()==63);sink.finish(7);CHECK(sink.buffer.size()==56);
    sink.finish(13);CHECK(sink.buffer.size()==43);sink.finish(43);
    CHECK(sink.buffer.size()==63);sink.finish(1);CHECK(sink.buffer.size()==62);
    drain(sink);CHECK(sink.wire==expected && !disconnects[1]);
}
void test_synchronous_errors() {
    {
        reset_queues();Source source;Sink sink;source.synchronous_error=true;
        rehab::platform::Stream stream{0,source,sink};stream.start();
        CHECK(source.starts==1 && source.maximum_depth==1 && disconnects[0]==1);
        for(unsigned i=0;i<10;++i)stream.poll();
        CHECK(source.starts==1 && sink.starts==0);
    }
    {
        reset_queues();Source source;Sink sink;sink.synchronous_error=true;
        rehab::platform::Stream stream{0,source,sink};outbound[0]=sequence(100);stream.start();
        CHECK(sink.starts==1 && sink.maximum_depth==1 && disconnects[0]==1);
        for(unsigned i=0;i<10;++i)stream.poll();
        CHECK(sink.starts==1);
        source.finish({},fibre::kStreamClosed);
        sink.synchronous_error=false;stream.start();outbound[0]=sequence(3,90);stream.poll();drain(sink);
        CHECK(sink.wire==sequence(3,90));
    }
}
void test_closed_callbacks_then_reconnect() {
    reset_queues();Source source;Sink sink;rehab::platform::Stream stream{0,source,sink};
    outbound[0]=sequence(140,1);stream.start();sink.finish(5);
    stream.stop();CHECK(disconnects[0]==1);
    // The native USB disconnect event marks both endpoints closed, then drains
    // their outstanding completion callbacks before a later connect event.
    sink.finish(0,fibre::kStreamClosed);source.finish(sequence(10,60),fibre::kStreamClosed);
    const auto prior=sink.wire;CHECK(inbound[0].empty());
    stream.start();CHECK(source.pending);
    const auto fresh=sequence(79,151);outbound[0]=fresh;stream.poll();drain(sink);
    std::vector<uint8_t> expected=prior;expected.insert(expected.end(),fresh.begin(),fresh.end());
    CHECK(sink.wire==expected);
    source.finish(sequence(5,90));CHECK(inbound[0]==sequence(5,90));
}
void test_successful_late_callbacks_then_reconnect() {
    reset_queues();Source source;Sink sink;rehab::platform::Stream stream{1,source,sink};
    outbound[1]=sequence(100,1);stream.start();stream.stop();
    // UART TX completion can still report OK after the stream was stopped by
    // an RX failure. A partial late completion must not carry the old tail
    // into the next connection/session.
    sink.finish(5);source.finish(sequence(5,70));
    CHECK(inbound[1].empty());const auto prior=sink.wire;
    stream.start();const auto fresh=sequence(11,181);outbound[1]=fresh;stream.poll();drain(sink);
    auto expected=prior;expected.insert(expected.end(),fresh.begin(),fresh.end());
    CHECK(sink.wire==expected);
}
void test_zero_progress_completion() {
    reset_queues();Source source;Sink sink;rehab::platform::Stream stream{0,source,sink};
    outbound[0]=sequence(10);stream.start();sink.finish(0);
    CHECK(disconnects[0]==1 && sink.starts==1 && !sink.pending);
    stream.poll();CHECK(sink.starts==1);
    source.finish({},fibre::kStreamClosed);stream.start();
    outbound[0]=sequence(4,99);stream.poll();drain(sink);
    CHECK(sink.wire==sequence(4,99));
}
void test_reconnect_before_retired_callbacks() {
    for(bool read_first:{false,true})for(auto status:{fibre::kStreamOk,fibre::kStreamClosed}) {
        reset_queues();Source source;Sink sink;rehab::platform::Stream stream{0,source,sink};
        outbound[0]=sequence(137,3);stream.start();
        CHECK(source.starts==1 && sink.starts==1 && source.pending && sink.pending);
        stream.stop();stream.start();
        const auto fresh_tx=sequence(91,101);outbound[0]=fresh_tx;stream.poll();
        // Old buffers remain owned by their old operations, so reconnect must
        // neither reuse them early nor register duplicate transfers.
        CHECK(source.starts==1 && sink.starts==1);
        if(read_first) {
            source.finish(sequence(8,221),status);
            CHECK(inbound[0].empty() && source.starts==2 && sink.starts==1);
            sink.finish(status==fibre::kStreamOk?7:0,status);
        } else {
            sink.finish(status==fibre::kStreamOk?7:0,status);
            CHECK(source.starts==1 && sink.starts==2);
            source.finish(sequence(8,221),status);
        }
        CHECK(inbound[0].empty() && source.starts==2 && disconnects[0]==1);
        auto expected=sink.wire;expected.insert(expected.end(),fresh_tx.begin(),fresh_tx.end());
        drain(sink);CHECK(sink.wire==expected);
        const auto fresh_rx=sequence(9,173);source.finish(fresh_rx);
        CHECK(inbound[0]==fresh_rx && source.starts==3 && disconnects[0]==1);
    }
}
}
int main() {
    struct Test {const char* name;void(*run)();} tests[]={
        {"start/poll wake",test_start_and_wake},{"USB 63-byte boundaries",test_usb_chunk_boundaries},
        {"partial completions",test_partial_completion},{"synchronous errors",test_synchronous_errors},
        {"closed callbacks/reconnect",test_closed_callbacks_then_reconnect},
        {"late successful callbacks/reconnect",test_successful_late_callbacks_then_reconnect},
        {"zero-progress completion",test_zero_progress_completion},
        {"reconnect before old callbacks",test_reconnect_before_retired_callbacks}
    };
    for(const auto& test:tests)try {test.run();std::printf("PASS %s\n",test.name);}
        catch(const TestFailure&) {std::printf("FAIL %s\n",test.name);}
    std::printf("stream integration: %u checks, %u failures (actual stream.cpp/Fibre async interfaces)\n",checks,failures);
    return failures?1:0;
}
