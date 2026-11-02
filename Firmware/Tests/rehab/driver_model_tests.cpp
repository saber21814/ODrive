// Host tests compile the actual V1.2 arbiter/HX711/CH394 sources against these
// deliberately small HAL models. They do not validate electrical timing.
#include "stm32_spi_arbiter.hpp"
#include "stm32_system.h"
#include "Rehab/drivers/driver_helpers.hpp"
#include "Rehab/drivers/ch394.hpp"
#include "Rehab/drivers/hx711.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

GPIO_TypeDef mock_gpio_a, mock_gpio_b, mock_gpio_c;
SPI_TypeDef mock_spi3, mock_spi1;
DMA_Stream_TypeDef mock_streams[8], mock_dma2_streams[8];
MockDwt mock_dwt;
MockCoreDebug mock_debug;
MockNvic mock_nvic;
uint32_t mock_primask = 0, mock_ipsr = 0, mock_flags_cleared = 0;
uint32_t mock_cycle_step = 1;
static uint32_t now_ms = 0;
static unsigned checks = 0, starts = 0, reinitializations = 0, deinitializations = 0;
static bool fail_start = false, auto_complete = false, network_mode = false, command_stuck = false;
static SPI_HandleTypeDef* pending_spis[2]{};
static unsigned bus_reinitializations[2]{}, bus_deinitializations[2]{};
static Stm32SpiArbiter* launch_race = nullptr;
static Stm32SpiArbiter* completion_commit_race = nullptr;
static std::vector<uint8_t> start_order;
static uint8_t common_regs[64], socket_regs[8][64], network_rx[4096], network_tx[4096];
static uint16_t rx_consumed_pointer = 0;
static size_t last_dma_bytes = 0;
static uint8_t* last_dma_tx = nullptr;

#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

bool Stm32Gpio::config(uint32_t, uint32_t, uint32_t) { return true; }
uint32_t HAL_GetTick() { return now_ms; }
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef* spi) {
    CHECK(!mock_ipsr); ++deinitializations; ++bus_deinitializations[spi->Instance == SPI1]; return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef* spi) {
    CHECK(!mock_ipsr); ++reinitializations; ++bus_reinitializations[spi->Instance == SPI1];
    spi->hdmatx->State = spi->hdmarx->State = HAL_DMA_STATE_READY;
    return HAL_OK;
}
static void complete_dma(SPI_HandleTypeDef* selected = nullptr) {
    SPI_HandleTypeDef* spi = selected ? selected : (pending_spis[0] ? pending_spis[0] : pending_spis[1]);
    if (!spi || pending_spis[spi->Instance == SPI1] != spi) return;
    pending_spis[spi->Instance == SPI1] = nullptr;
    spi->hdmatx->Instance->CR = spi->hdmarx->Instance->CR = 0;
    spi->hdmarx->State = HAL_DMA_STATE_READY;
    const uint32_t previous = mock_ipsr;
    mock_ipsr = 16 + (spi->Instance == SPI1 ? DMA2_Stream2_IRQn : DMA1_Stream0_IRQn);
    if (spi->hdmarx->XferCpltCallback) spi->hdmarx->XferCpltCallback(spi->hdmarx);
    mock_ipsr = previous;
}
void osDelay(unsigned time) { now_ms += time; if (auto_complete) complete_dma(); }
static void network_transfer(uint8_t* tx, uint8_t* rx, uint16_t size) {
    using namespace rehab::drivers;
    const uint16_t address = read_be16(tx);
    const uint8_t block = tx[2] & 0xf8u, sock = block >> 5;
    const bool write = tx[2] & 4;
    uint8_t* base = nullptr;
    size_t capacity = 0;
    if (!(block & 0x18)) { base = common_regs; capacity = sizeof(common_regs); }
    else if ((block & 0x18) == 8) { base = socket_regs[sock]; capacity = sizeof(socket_regs[0]); }
    else if ((block & 0x18) == 16) { base = network_tx; capacity = sizeof(network_tx); }
    else { base = network_rx; capacity = sizeof(network_rx); }
    CHECK(static_cast<size_t>(address) + size - 3 <= capacity);
    for (uint16_t i = 3; i < size; ++i) {
        const size_t offset = address + i - 3;
        if (write) {
            if (block == 8 && offset == 2) base[offset] &= ~tx[i];
            else base[offset] = tx[i];
        } else rx[i] = base[offset];
    }
    if (write && block == 8 && address == 1 && !command_stuck) {
        const uint8_t cmd = tx[3];
        socket_regs[0][1] = 0;
        if (cmd == 1) socket_regs[0][3] = 0x13;
        if (cmd == 2) socket_regs[0][3] = 0x14;
        if (cmd == 0x10) socket_regs[0][3] = 0;
        if (cmd == 0x20) socket_regs[0][2] |= 0x10;
        if (cmd == 0x40) {
            const uint16_t pointer = read_be16(socket_regs[0] + 0x28);
            const uint16_t consumed = static_cast<uint16_t>(pointer - rx_consumed_pointer);
            write_be16(socket_regs[0] + 0x26, static_cast<uint16_t>(read_be16(socket_regs[0] + 0x26) - consumed));
            rx_consumed_pointer = pointer;
        }
    }
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef* spi, uint8_t* tx, uint8_t* rx, uint16_t length) {
    CHECK(!mock_primask); // motor interrupts must remain enabled during launch
    ++starts;
    if (fail_start) { fail_start = false; return HAL_ERROR; }
    last_dma_bytes = length * (spi->Init.DataSize == SPI_DATASIZE_16BIT ? 2u : 1u);
    last_dma_tx = tx;
    start_order.push_back(tx[0]);
    spi->hdmatx->State = spi->hdmarx->State = 1;
    spi->hdmatx->Instance->CR = spi->hdmarx->Instance->CR = DMA_SxCR_EN;
    spi->Instance->CR2 = SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN;
    pending_spis[spi->Instance == SPI1] = spi;
    if (network_mode) network_transfer(tx, rx, length);
    else for (size_t i = 0; i < last_dma_bytes; ++i) rx[i] = tx[i] ^ 0x55u;
    if (launch_race) { auto* arbiter = launch_race; launch_race = nullptr; arbiter->on_error(); arbiter->service(now_ms); }
    return HAL_OK;
}

struct Fixture {
    DMA_HandleTypeDef tx{}, rx{};
    SPI_HandleTypeDef spi{};
    Stm32SpiArbiter arbiter{&spi};
    Fixture() {
        mock_spi3 = mock_spi1 = SPI_TypeDef{};
        for (auto& stream : mock_streams) stream = DMA_Stream_TypeDef{};
        for (auto& stream : mock_dma2_streams) stream = DMA_Stream_TypeDef{};
        now_ms = starts = mock_flags_cleared = mock_primask = mock_ipsr = 0;
        reinitializations = deinitializations = 0;
        for (unsigned i = 0; i < 2; ++i) bus_reinitializations[i] = bus_deinitializations[i] = 0;
        mock_cycle_step = 8; mock_dwt_running = true; mock_gpio_bsrr_hook = nullptr;
        fail_start = auto_complete = network_mode = command_stuck = false;
        launch_race = nullptr; pending_spis[0] = pending_spis[1] = nullptr; start_order.clear();
        completion_commit_race = nullptr; CriticalSectionContext::before_enter = nullptr;
        tx.Instance = DMA1_Stream7; rx.Instance = DMA1_Stream0;
        tx.Parent = rx.Parent = &spi;
        spi.Instance = SPI3; spi.hdmatx = &tx; spi.hdmarx = &rx;
        mock_nvic = MockNvic{};
        NVIC_EnableIRQ(DMA1_Stream7_IRQn); NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    }
};
struct SecondBus {
    DMA_HandleTypeDef tx{}, rx{};
    SPI_HandleTypeDef spi{};
    Stm32SpiArbiter arbiter{&spi};
    SecondBus() {
        tx.Instance = DMA2_Stream3; rx.Instance = DMA2_Stream2;
        tx.Init.Channel = rx.Init.Channel = DMA_CHANNEL_3;
        tx.Parent = rx.Parent = &spi;
        spi.Instance = SPI1; spi.hdmatx = &tx; spi.hdmarx = &rx;
        NVIC_EnableIRQ(DMA2_Stream2_IRQn); NVIC_EnableIRQ(DMA2_Stream3_IRQn);
    }
};
struct Completion { unsigned count = 0; bool success = false; };
static Stm32SpiArbiter::SpiTask task(uint8_t* tx, uint8_t* rx, Completion& done) {
    Stm32SpiArbiter::SpiTask t{};
    t.ncs_gpio = Stm32Gpio{GPIOC, GPIO_PIN_2}; t.tx_buf = tx; t.rx_buf = rx;
    t.length = 1; t.timeout_ms = 10;
    t.on_complete_ctx = &done;
    t.on_complete = [](void* context, bool success) {
        auto& c = *static_cast<Completion*>(context); ++c.count; c.success = success;
    };
    return t;
}

static void test_helpers() {
    using namespace rehab::drivers;
    CHECK(sign_extend_24(0x7fffff) == 8388607);
    CHECK(sign_extend_24(0x800000) == -8388608);
    CHECK(sign_extend_24(0xffffffff) == -1);
    CHECK(sign_extend_24(0x123456) == 1193046);
    CHECK(ring_chunk(4095, 256) == 1);
    CHECK(ring_chunk(65535, 256) == 1);
    CHECK(ring_chunk(4096, 256) == 256);
    ByteQueue<4> queue;
    uint8_t in[] = {1,2,3,4,5}, out[5]{};
    CHECK(queue.push(in, 5) == 4); CHECK(queue.pop(out, 3) == 3);
    CHECK(queue.push(in, 3) == 3); CHECK(queue.pop(out, 5) == 4);
    CHECK(out[0] == 4 && out[1] == 1 && out[3] == 3);
}
static void test_arbiter() {
    {
        Fixture f; uint8_t in[2]{0x32,0x76}, out[2]{}; Completion done;
        auto t = task(in,out,done); t.config.DataSize = SPI_DATASIZE_16BIT;
        CHECK(Stm32SpiArbiter::acquire_task(&t)); CHECK(!Stm32SpiArbiter::acquire_task(&t));
        Stm32SpiArbiter::release_task(&t); CHECK(Stm32SpiArbiter::acquire_task(&t));
        f.arbiter.transfer_async(&t);
        CHECK(last_dma_bytes == 2 && last_dma_tx != in && done.count == 0);
        complete_dma(); CHECK(done.count == 1 && done.success && out[0] == (0x32 ^ 0x55) && out[1] == (0x76 ^ 0x55));
        f.arbiter.on_complete(); CHECK(done.count == 1);
    }
    {
        Fixture f; uint8_t in[3]{1,2,3}; Completion done[3];
        auto a = task(in,nullptr,done[0]), b = task(in+1,nullptr,done[1]), c = task(in+2,nullptr,done[2]);
        a.priority = b.priority = 3;
        f.arbiter.transfer_async(&a); f.arbiter.transfer_async(&b); f.arbiter.transfer_async(&c);
        complete_dma(); complete_dma(); complete_dma();
        CHECK(start_order == std::vector<uint8_t>({1,3,2}));
        CHECK(done[0].count == 1 && done[1].count == 1 && done[2].count == 1);
    }
    {
        Fixture f; uint8_t in = 1; Completion done[10]; Stm32SpiArbiter::SpiTask list[10];
        for (unsigned i = 0; i < 10; ++i) { list[i] = task(&in,nullptr,done[i]); f.arbiter.transfer_async(&list[i]); }
        CHECK(done[9].count == 1 && !done[9].success && f.arbiter.diagnostics().highwater == 8);
        for (unsigned i = 0; i < 9; ++i) complete_dma();
        for (auto& c : done) CHECK(c.count == 1);
    }
    {
        Fixture f; uint8_t in = 1; Completion done; auto t = task(&in,nullptr,done);
        fail_start = true; f.arbiter.transfer_async(&t);
        CHECK(done.count == 1 && !done.success);
        f.arbiter.transfer_async(&t); complete_dma(); CHECK(done.count == 2 && done.success);
    }
    {
        Fixture f; uint8_t in = 1, out = 0xab; Completion done; auto t = task(&in,&out,done);
        f.arbiter.transfer_async(&t); now_ms = 10;
        mock_nvic.pending = (uint64_t{1} << DMA1_Stream0_IRQn) | (uint64_t{1} << DMA1_Stream7_IRQn) | (uint64_t{1} << SPI3_IRQn);
        f.arbiter.service(now_ms);
        CHECK(done.count == 1 && !done.success && out == 0xab);
        CHECK(!(f.tx.Instance->CR & DMA_SxCR_EN) && !(f.rx.Instance->CR & DMA_SxCR_EN));
        CHECK(!mock_nvic.pending && mock_flags_cleared == 31);
        CHECK(!(mock_nvic.ISER[SPI3_IRQn >> 5] & (1u << (SPI3_IRQn & 31))));
        f.arbiter.on_complete(); f.arbiter.on_error(); CHECK(done.count == 1);
        f.arbiter.transfer_async(&t); complete_dma(); CHECK(done.count == 2 && done.success);
    }
    {
        Fixture f; uint8_t in = 1; Completion done; auto t = task(&in,nullptr,done);
        launch_race = &f.arbiter; f.arbiter.transfer_async(&t);
        CHECK(done.count == 1 && !done.success && !f.arbiter.diagnostics().quarantined);
        f.arbiter.transfer_async(&t); complete_dma(); CHECK(done.count == 2 && done.success);
    }
    {
        Fixture f; uint8_t in = 1, out = 0xee;
        CHECK(!f.arbiter.transfer({}, Stm32Gpio{GPIOC,GPIO_PIN_2}, &in,&out,1,3));
        CHECK(now_ms == 3 && out == 0xee && !(f.rx.Instance->CR & DMA_SxCR_EN));
        // Callback/context belonged to the retired stack frame; this must be inert.
        complete_dma(); CHECK(out == 0xee);
        auto_complete = true;
        CHECK(f.arbiter.transfer({}, Stm32Gpio{GPIOC,GPIO_PIN_2}, &in,&out,1,3));
    }
    {
        Fixture f; uint8_t in[260]{}; Completion done; auto t = task(in,nullptr,done);
        t.length = 131; t.config.DataSize = SPI_DATASIZE_16BIT; f.arbiter.transfer_async(&t);
        CHECK(done.count == 1 && !done.success && starts == 0);
        mock_ipsr = 4; CHECK(!f.arbiter.transfer({}, Stm32Gpio{GPIOC,GPIO_PIN_2},in,nullptr,1,2)); mock_ipsr = 0;
    }
    {
        Fixture f; uint8_t in = 1; Completion done; auto t = task(&in,nullptr,done);
        struct Reentry { Stm32SpiArbiter* arbiter; Stm32SpiArbiter::SpiTask* task; unsigned count; } context{&f.arbiter,&t,0};
        t.on_complete_ctx = &context;
        t.on_complete = [](void* raw, bool success) {
            auto& c = *static_cast<Reentry*>(raw); CHECK(success); ++c.count;
            Stm32SpiArbiter::release_task(c.task);
            if (c.count == 1) {
                CHECK(Stm32SpiArbiter::acquire_task(c.task));
                c.task->tx_buf = nullptr; // reuse same descriptor as RX-only
                static uint8_t result;
                c.task->rx_buf = &result;
                c.arbiter->transfer_async(c.task);
            }
        };
        f.arbiter.transfer_async(&t); complete_dma(); complete_dma();
        CHECK(context.count == 2 && starts == 2);
    }
    {
        Fixture f; uint8_t in = 1; Completion active, queued;
        auto a = task(&in,nullptr,active), b = task(&in,nullptr,queued);
        a.timeout_ms = 100; b.timeout_ms = 2;
        f.arbiter.transfer_async(&a); f.arbiter.transfer_async(&b); now_ms = 3; f.arbiter.service(now_ms);
        CHECK(queued.count == 1 && !queued.success && active.count == 0);
        CHECK(starts == 1 && f.arbiter.diagnostics().timeouts == 1);
        complete_dma(); CHECK(active.count == 1 && active.success);
    }
    {
        Fixture f; uint8_t out = 0; Completion done; auto t = task(nullptr,&out,done);
        f.arbiter.transfer_async(&t); complete_dma(); CHECK(done.success && out == 0xaa);
        f.arbiter.transfer_async(&t); f.spi.Instance->SR |= SPI_SR_BSY; complete_dma();
        CHECK(done.count == 1); f.arbiter.service(now_ms); CHECK(done.count == 2 && !done.success);
        f.spi.Instance->SR = 0;
        f.arbiter.transfer_async(&t); f.rx.ErrorCode = 4; complete_dma();
        CHECK(done.count == 2); f.arbiter.service(now_ms); CHECK(done.count == 3 && !done.success);
    }
    {
        Fixture f; uint8_t in = 1; Completion done; auto t = task(&in,nullptr,done);
        now_ms = 0xfffffffcu; t.timeout_ms = 5; f.arbiter.transfer_async(&t);
        now_ms = 1; f.arbiter.service(now_ms); CHECK(done.count == 1 && !done.success);
    }
    {
        Fixture f; uint8_t in[2]{1,2}; Completion a_done,b_done;
        auto a=task(in,nullptr,a_done), b=task(in,nullptr,b_done);
        b.config.DataSize=SPI_DATASIZE_16BIT;
        f.arbiter.transfer_async(&a); f.arbiter.transfer_async(&b);
        complete_dma(); // the callback runs in the DMA IRQ; b needs a mode change
        CHECK(a_done.count==1 && a_done.success && b_done.count==0);
        CHECK(starts==1 && reinitializations==0 && deinitializations==0);
        f.arbiter.service(now_ms);
        CHECK(starts==2 && reinitializations==1 && deinitializations==1 && last_dma_bytes==2);
        complete_dma(); CHECK(b_done.count==1 && b_done.success);
    }
    {
        Fixture f; uint8_t in=1; Completion done; auto t=task(&in,nullptr,done);
        t.config.CLKPhase=SPI_PHASE_2EDGE;
        mock_ipsr=16+DMA1_Stream0_IRQn; f.arbiter.transfer_async(&t); mock_ipsr=0;
        CHECK(starts==0 && done.count==0 && reinitializations==0);
        f.arbiter.service(now_ms); CHECK(starts==1 && reinitializations==1);
        complete_dma(); CHECK(done.count==1 && done.success);
    }
    {
        Fixture f; uint8_t in=1; Completion done; auto t=task(&in,nullptr,done);
        f.arbiter.transfer_async(&t); now_ms=10; f.arbiter.service(now_ms);
        CHECK(done.count==1 && !done.success && reinitializations==0);
        // The config is unchanged, but fault recovery still requires a HAL
        // reinit. An IRQ producer must enqueue without executing that reinit.
        mock_ipsr=16+DMA1_Stream0_IRQn; f.arbiter.transfer_async(&t); mock_ipsr=0;
        CHECK(starts==1 && done.count==1 && reinitializations==0);
        f.arbiter.service(now_ms); CHECK(starts==2 && reinitializations==1);
        complete_dma(); CHECK(done.count==2 && done.success);
    }
}
static uint64_t irq_bit(IRQn_Type irq) { return uint64_t{1} << irq; }
static bool irq_enabled(IRQn_Type irq) { return mock_nvic.ISER[irq >> 5] & (1u << (irq & 31)); }
static void prepare_adc() {
    DMA2_Stream0->CR = DMA_SxCR_EN | DMA_SxCR_TCIE;
    DMA2_Stream0->FCR = DMA_SxFCR_FEIE;
    DMA2_Stream0->cleared_flags = 0;
    NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    mock_nvic.pending |= irq_bit(DMA2_Stream0_IRQn);
}
static void check_adc_untouched() {
    CHECK(DMA2_Stream0->CR == (DMA_SxCR_EN | DMA_SxCR_TCIE));
    CHECK(DMA2_Stream0->FCR == DMA_SxFCR_FEIE && DMA2_Stream0->cleared_flags == 0);
    CHECK(irq_enabled(DMA2_Stream0_IRQn) && (mock_nvic.pending & irq_bit(DMA2_Stream0_IRQn)));
}
static void test_dual_spi() {
    for(unsigned failing_bus=0;failing_bus<2;++failing_bus) {
        Fixture f; SecondBus n; prepare_adc();
        uint8_t tx=0x42,out=0xa5; Completion fd,nd;
        auto a=task(&tx,&out,fd),b=task(&tx,nullptr,nd);
        f.arbiter.transfer_async(&a);n.arbiter.transfer_async(&b);
        auto& bad=failing_bus?n.arbiter:f.arbiter;
        auto& good=failing_bus?f.arbiter:n.arbiter;
        auto& failed=failing_bus?nd:fd;
        auto& other=failing_bus?fd:nd;
        completion_commit_race=&bad;
        // First lock is on_complete's check; second is finish's commit. Model
        // TX priority3 preempting RX priority4 precisely between those locks.
        CriticalSectionContext::before_enter=[] {
            CriticalSectionContext::before_enter=[] { completion_commit_race->on_error(); };
        };
        complete_dma(failing_bus?&n.spi:&f.spi);
        CHECK(failed.count==0 && other.count==0);
        CHECK((failing_bus?f.rx:n.rx).Instance->CR&DMA_SxCR_EN);
        bad.service(now_ms);
        CHECK(failed.count==1 && !failed.success && other.count==0);
        if(!failing_bus)CHECK(out==0xa5);
        CHECK(bad.diagnostics().errors==1 && good.diagnostics().errors==0);
        complete_dma(failing_bus?&f.spi:&n.spi);
        CHECK(other.count==1 && other.success);
        check_adc_untouched();
    }
    {
        Fixture f; SecondBus n; prepare_adc();
        uint8_t tx[4]{0x11,0x22,0x33,0x44}, rx[4]{}; Completion done[4];
        auto a=task(tx,rx,done[0]), b=task(tx+1,rx+1,done[1]);
        auto c=task(tx+2,rx+2,done[2]), d=task(tx+3,rx+3,done[3]);
        a.ncs_gpio=c.ncs_gpio=Stm32Gpio{GPIOC,uint16_t(1u<<13)};
        f.arbiter.transfer_async(&a); n.arbiter.transfer_async(&b);
        f.arbiter.transfer_async(&c); n.arbiter.transfer_async(&d);
        CHECK(starts==2 && pending_spis[0]==&f.spi && pending_spis[1]==&n.spi);
        complete_dma(&n.spi);
        CHECK(done[1].count==1 && done[1].success && done[0].count==0 && starts==3);
        CHECK((f.rx.Instance->CR&DMA_SxCR_EN) && (n.rx.Instance->CR&DMA_SxCR_EN));
        complete_dma(&f.spi); complete_dma(&n.spi); complete_dma(&f.spi);
        for(unsigned i=0;i<4;++i) CHECK(done[i].count==1 && done[i].success && rx[i]==(tx[i]^0x55));
        CHECK(start_order==std::vector<uint8_t>({0x11,0x22,0x44,0x33}));
        CHECK(!f.arbiter.diagnostics().errors && !n.arbiter.diagnostics().errors);
        check_adc_untouched();
    }
    {
        Fixture f; SecondBus n; prepare_adc();
        uint8_t tx[2]{1,2}, out[2]{0xaa,0xbb}; Completion fd,nd;
        auto a=task(tx,out,fd), b=task(tx+1,out+1,nd); a.timeout_ms=100; b.timeout_ms=2;
        f.arbiter.transfer_async(&a); n.arbiter.transfer_async(&b);
        auto late_rx=n.rx.XferCpltCallback; auto late_error=n.tx.XferErrorCallback;
        f.rx.Instance->cleared_flags=f.tx.Instance->cleared_flags=0;
        NVIC_EnableIRQ(SPI1_IRQn); NVIC_EnableIRQ(SPI3_IRQn);
        const auto f_pending=irq_bit(DMA1_Stream0_IRQn)|irq_bit(DMA1_Stream7_IRQn)|irq_bit(SPI3_IRQn);
        const auto n_pending=irq_bit(DMA2_Stream2_IRQn)|irq_bit(DMA2_Stream3_IRQn)|irq_bit(SPI1_IRQn);
        mock_nvic.pending |= f_pending|n_pending;
        now_ms=2; n.arbiter.service(now_ms);
        CHECK(nd.count==1 && !nd.success && out[1]==0xbb && fd.count==0);
        CHECK(f.spi.Instance->CR2==(SPI_CR2_TXDMAEN|SPI_CR2_RXDMAEN));
        CHECK((f.rx.Instance->CR&DMA_SxCR_EN) && (f.tx.Instance->CR&DMA_SxCR_EN));
        CHECK(!f.rx.Instance->cleared_flags && !f.tx.Instance->cleared_flags);
        CHECK((mock_nvic.pending&f_pending)==f_pending && !(mock_nvic.pending&n_pending));
        CHECK(irq_enabled(SPI1_IRQn) && irq_enabled(SPI3_IRQn)); check_adc_untouched();
        // Retired callbacks are inert; launch clears only this bus's old bits.
        late_rx(&n.rx); late_error(&n.tx); CHECK(nd.count==1 && fd.count==0);
        mock_nvic.pending |= n_pending & ~irq_bit(SPI1_IRQn);
        n.arbiter.transfer_async(&b);
        CHECK(bus_reinitializations[1]==1 && bus_reinitializations[0]==0);
        CHECK(bus_deinitializations[1]==1 && bus_deinitializations[0]==0);
        CHECK(!(mock_nvic.pending & (n_pending & ~irq_bit(SPI1_IRQn))));
        late_rx(&n.rx); late_error(&n.tx);
        CHECK(nd.count==1 && fd.count==0 && n.spi.Instance->CR2==(SPI_CR2_TXDMAEN|SPI_CR2_RXDMAEN));
        f.arbiter.service(now_ms); CHECK(fd.count==0);
        complete_dma(&f.spi); complete_dma(&n.spi);
        CHECK(fd.count==1 && fd.success && nd.count==2 && nd.success);
        CHECK(!f.arbiter.diagnostics().errors && n.arbiter.diagnostics().timeouts==1);
        check_adc_untouched();
    }
    {
        Fixture f; SecondBus n; prepare_adc();
        uint8_t tx=7; Completion fd,nd; auto a=task(&tx,nullptr,fd), b=task(&tx,nullptr,nd);
        f.arbiter.transfer_async(&a); n.arbiter.transfer_async(&b);
        n.rx.Instance->cleared_flags=n.tx.Instance->cleared_flags=0;
        f.tx.ErrorCode=4; f.tx.XferErrorCallback(&f.tx);
        CHECK(fd.count==0 && nd.count==0 && (f.rx.Instance->CR&DMA_SxCR_EN));
        f.arbiter.service(now_ms);
        CHECK(fd.count==1 && !fd.success && nd.count==0);
        CHECK((n.rx.Instance->CR&DMA_SxCR_EN) && (n.tx.Instance->CR&DMA_SxCR_EN));
        CHECK(!n.rx.Instance->cleared_flags && !n.tx.Instance->cleared_flags);
        complete_dma(&n.spi); CHECK(nd.count==1 && nd.success);
        f.arbiter.transfer_async(&a); complete_dma(&f.spi);
        CHECK(fd.count==2 && fd.success && bus_reinitializations[0]==1 && bus_reinitializations[1]==0);
        check_adc_untouched();
    }
    for(unsigned invalid=0;invalid<6;++invalid) {
        Fixture f; SecondBus n; prepare_adc();
        uint8_t tx=3; Completion fd,nd; auto a=task(&tx,nullptr,fd), b=task(&tx,nullptr,nd);
        f.arbiter.transfer_async(&a);
        SPI_TypeDef unsupported{};
        if(invalid==0) n.spi.Instance=&unsupported;
        if(invalid==1) n.rx.Instance=DMA2_Stream0;
        if(invalid==2) n.tx.Instance=DMA1_Stream7;
        if(invalid==3) n.rx.Init.Channel=DMA_CHANNEL_0;
        if(invalid==4) n.tx.Parent=&f.spi;
        if(invalid==5) n.spi.hdmarx=nullptr;
        const MockNvic previous=mock_nvic;
        const uint32_t prior_flags=mock_flags_cleared;
        n.arbiter.transfer_async(&b); n.arbiter.service(now_ms);
        CHECK(nd.count==1 && !nd.success && n.arbiter.diagnostics().rejected==1 && starts==1);
        CHECK(std::memcmp(&mock_nvic,&previous,sizeof(previous))==0 && mock_flags_cleared==prior_flags);
        CHECK(f.rx.Instance->CR&DMA_SxCR_EN); check_adc_untouched();
        complete_dma(&f.spi); CHECK(fd.count==1 && fd.success);
    }
    {
        Fixture f; SecondBus n; uint8_t tx=4; Completion fd,nd,other_done;
        auto a=task(&tx,nullptr,fd), b=task(&tx,nullptr,nd), other=task(&tx,nullptr,other_done);
        f.arbiter.transfer_async(&a); n.arbiter.transfer_async(&b);
        Stm32SpiArbiter duplicate{&f.spi};
        duplicate.transfer_async(&other);
        CHECK(other_done.count==1 && !other_done.success && starts==2);
        complete_dma(&f.spi); duplicate.transfer_async(&other);
        CHECK(other_done.count==2 && !other_done.success && starts==2);
        complete_dma(&n.spi); CHECK(fd.success && nd.success);
    }
    {
        // Even an externally mutated handle cannot move a registered owner to
        // another physical bus. No hardware access is made for that request.
        Fixture f; uint8_t tx=4; Completion first,rejected;
        auto a=task(&tx,nullptr,first), b=task(&tx,nullptr,rejected);
        f.arbiter.transfer_async(&a); complete_dma(&f.spi); CHECK(first.success);
        DMA_HandleTypeDef tx1{},rx1{};
        tx1.Instance=DMA2_Stream3; rx1.Instance=DMA2_Stream2;
        tx1.Init.Channel=rx1.Init.Channel=DMA_CHANNEL_3;
        tx1.Parent=rx1.Parent=&f.spi;
        f.spi.Instance=SPI1; f.spi.hdmatx=&tx1; f.spi.hdmarx=&rx1;
        const MockNvic previous=mock_nvic;
        f.arbiter.transfer_async(&b);
        CHECK(rejected.count==1 && !rejected.success && starts==1);
        CHECK(std::memcmp(&mock_nvic,&previous,sizeof(previous))==0);
    }

}
static uint32_t hx_bits = 0;
static unsigned hx_clocks = 0, hx_total_clocks = 0;
static bool hx_stuck_low = false, hx_unmasked_high = false;
static void hx_gpio_write(uint32_t value) {
    if (value & GPIO_PIN_7) {
        mock_gpio_c.IDR |= GPIO_PIN_7;
        ++hx_clocks; ++hx_total_clocks;
        if (!mock_primask) hx_unmasked_high = true;
        const bool bit = !hx_stuck_low && (hx_clocks <= 24 ? ((hx_bits >> (24 - hx_clocks)) & 1u) : true);
        if (bit) mock_gpio_c.IDR |= GPIO_PIN_6; else mock_gpio_c.IDR &= ~GPIO_PIN_6;
    }
    if (value & (GPIO_PIN_7 << 16)) mock_gpio_c.IDR &= ~GPIO_PIN_7;
}
static void hx_ready(uint32_t bits = 0, bool stuck_low = false) {
    hx_bits=bits; hx_clocks=0; hx_stuck_low=stuck_low;
    mock_gpio_c.IDR &= ~GPIO_PIN_6;
}
static void test_hx711() {
    Fixture f; rehab::drivers::Hx711 hx;
    hx_total_clocks=0; hx_unmasked_high=false; mock_gpio_bsrr_hook=hx_gpio_write;
    CHECK(hx.init(true));
    for(uint32_t time=0;time<50000;time+=12500) { hx_ready(); hx.poll(time); CHECK(!hx.sample().valid); }
    hx_ready(0x800123); hx.poll(50000);
    CHECK(hx.sample().valid && hx.sample().sequence==1 && hx.sample().raw==-8388317);
    CHECK(hx_clocks==25 && !hx_unmasked_high && !mock_primask);
    const unsigned before=hx_total_clocks;
    hx_ready(0x7fffff); hx.poll(51000); hx.poll(62499);
    CHECK(hx_total_clocks==before && hx.sample().sequence==1 && hx.sample().timestamp_us==50000);
    hx.poll(62500); CHECK(hx.sample().sequence==2 && hx.sample().timestamp_us==62500 && hx.sample().raw==8388607);
    // DOUT stays high until a conversion is ready: no polling-created samples.
    hx.poll(1000000); CHECK(hx.sample().sequence==2 && hx.sample().timestamp_us==62500);
    // A stuck-low wire cannot pass the required post-clock ready-to-busy edge.
    hx_ready(0,true); hx.poll(1012500);
    CHECK(!hx.sample().valid && hx.sample().sequence==2 && hx.sample().timestamp_us==62500);
    const unsigned stalled=hx_total_clocks;
    hx.poll(1013500); CHECK(hx_total_clocks==stalled && hx.sample().sequence==2);
    hx_ready(1234); hx.poll(1025000); CHECK(hx.sample().valid && hx.sample().sequence==3 && hx.sample().raw==1234);
    // A slow instruction/interrupt model exceeding the measured high limit
    // invalidates the conversion and restarts settling, with IRQ mask restored.
    mock_cycle_step=300; hx_ready(); hx.poll(1037500);
    CHECK(!hx.sample().valid && hx.timing_faults()==1 && !mock_primask);
    mock_cycle_step=8; hx.deinit(); CHECK(!hx.enabled() && !hx.sample().valid);

    CHECK(hx.init(false));
    for(uint32_t time=0;time<400000;time+=100000) { hx_ready(); hx.poll(time); CHECK(!hx.sample().valid); }
    hx_ready(0x123456); hx.poll(400000); CHECK(hx.sample().valid && hx.sample().sequence==1 && hx.sample().raw==1193046);
    const unsigned slow_before=hx_total_clocks;
    hx_ready(42); hx.poll(412500); hx.poll(499999);
    CHECK(hx_total_clocks==slow_before && hx.sample().sequence==1);
    hx.poll(500000); CHECK(hx.sample().sequence==2 && hx.sample().timestamp_us==500000 && hx.sample().raw==42);
    // Once DWT has stopped, both high and low spin waits still terminate. SCK
    // always returns low; no valid sample/sequence is manufactured.
    mock_dwt_running=false; hx_ready(); hx.poll(600000);
    CHECK(!hx.sample().valid && hx.sample().sequence==2 && hx.timing_faults()==2);
    CHECK(!mock_primask && !(mock_gpio_c.IDR&GPIO_PIN_7));
    mock_dwt_running=true; hx.deinit();
    // A stopped DWT at initialization refuses the driver before claiming pins.
    mock_dwt_running=false; CHECK(!hx.init(true) && !hx.enabled());
    mock_dwt_running=true; mock_gpio_bsrr_hook=nullptr;
}
static void advance(rehab::drivers::Ch394& chip, unsigned count) {
    for (unsigned i = 0; i < count; ++i) { ++now_ms; chip.poll(now_ms); }
}
static void test_network() {
    using namespace rehab::drivers;
    Fixture f; network_mode = auto_complete = true;
    std::memset(common_regs,0,sizeof(common_regs)); std::memset(socket_regs,0,sizeof(socket_regs));
    common_regs[0x39] = 4; common_regs[0x2e] = 1;
    write_be16(socket_regs[0] + 0x20,4096);
    Ch394 chip{f.arbiter}; NetworkConfig config{{192,168,4,2},{192,168,4,1},{255,255,255,0},39400};
    CHECK(!chip.init(NetworkConfig{})); CHECK(chip.init(config));
    advance(chip,120);
    CHECK(chip.status().state == Ch394::State::Monitor && chip.status().errors == 0);
    CHECK(socket_regs[0][0x1e] == 4 && socket_regs[0][0x1f] == 4 && socket_regs[7][0x1e] == 0);
    CHECK(read_be16(socket_regs[0] + 4) == 39400 && common_regs[0xf] == 192 && common_regs[0x12] == 2);
    socket_regs[0][3] = 0x17; advance(chip,4); CHECK(chip.connected());
    network_rx[4094] = 'A'; network_rx[4095] = 'B'; network_rx[0] = 'C'; network_rx[1] = 'D';
    rx_consumed_pointer = 4094;
    write_be16(socket_regs[0] + 0x28,4094); write_be16(socket_regs[0] + 0x26,4);
    advance(chip,16); uint8_t result[8]{};
    CHECK(chip.receive(result,sizeof(result)) == 4 && !std::memcmp(result,"ABCD",4));
    CHECK(read_be16(socket_regs[0] + 0x28) == 4098 && chip.status().rx_bytes == 4);
    write_be16(socket_regs[0] + 0x24,65534);
    CHECK(chip.send(reinterpret_cast<const uint8_t*>("EFGH"),4) == 4);
    advance(chip,20);
    CHECK(network_tx[4094] == 'E' && network_tx[4095] == 'F' && network_tx[0] == 'G' && network_tx[1] == 'H');
    CHECK(read_be16(socket_regs[0] + 0x24) == 2 && chip.status().tx_bytes == 4);
    socket_regs[0][3] = 0x1c; advance(chip,16);
    CHECK(!chip.connected() && socket_regs[0][3] == 0x14);
    socket_regs[0][3] = 0x17; advance(chip,8);
    write_be16(socket_regs[0] + 0x26,5000); advance(chip,8);
    CHECK(chip.status().state == Ch394::State::Backoff && chip.status().last_error == Ch394::Error::BadCount);
    chip.deinit(); CHECK(chip.status().state == Ch394::State::Disabled);
    CHECK(chip.init(config)); command_stuck = true; advance(chip,260);
    CHECK(chip.status().last_error == Ch394::Error::CommandTimeout);
}
int main() {
    test_helpers(); test_arbiter(); test_dual_spi(); test_hx711(); test_network();
    std::printf("driver model tests: %u checks passed (actual arbiter/HX711/CH394 sources)\n", checks);
}
