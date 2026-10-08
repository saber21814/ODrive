#include "stm32_spi_arbiter.hpp"
#include "stm32_system.h"
#include <cmsis_os.h>
#include <cstring>

#if !defined(REHAB_V12)
#error "V1.2 SPI arbiter must only be built for REHAB_V12"
#endif

namespace {
Stm32SpiArbiter* spi3_owner = nullptr;
bool same_config(const SPI_InitTypeDef& a, const SPI_InitTypeDef& b) {
    return a.Mode == b.Mode && a.Direction == b.Direction && a.DataSize == b.DataSize
        && a.CLKPolarity == b.CLKPolarity && a.CLKPhase == b.CLKPhase && a.NSS == b.NSS
        && a.BaudRatePrescaler == b.BaudRatePrescaler && a.FirstBit == b.FirstBit
        && a.TIMode == b.TIMode && a.CRCCalculation == b.CRCCalculation
        && a.CRCPolynomial == b.CRCPolynomial;
}
size_t byte_count(const Stm32SpiArbiter::SpiTask& t) {
    return t.length * (t.config.DataSize == SPI_DATASIZE_16BIT ? 2u : 1u);
}
IRQn_Type dma_irq(DMA_Stream_TypeDef* stream) {
    if (stream == DMA1_Stream0) return DMA1_Stream0_IRQn;
    if (stream == DMA1_Stream1) return DMA1_Stream1_IRQn;
    if (stream == DMA1_Stream2) return DMA1_Stream2_IRQn;
    if (stream == DMA1_Stream3) return DMA1_Stream3_IRQn;
    if (stream == DMA1_Stream4) return DMA1_Stream4_IRQn;
    if (stream == DMA1_Stream5) return DMA1_Stream5_IRQn;
    if (stream == DMA1_Stream6) return DMA1_Stream6_IRQn;
    return DMA1_Stream7_IRQn;
}
void callback(Stm32SpiArbiter::SpiTask* task, bool success) {
    // Read everything before the callback: it may immediately reuse/free task.
    const auto fn = task->on_complete;
    void* const ctx = task->on_complete_ctx;
    if (fn) fn(ctx, success);
}
void dma_error(DMA_HandleTypeDef*) {
    if (spi3_owner) spi3_owner->on_error();
}
void dma_complete(DMA_HandleTypeDef* dma) {
    auto* spi = static_cast<SPI_HandleTypeDef*>(dma->Parent);
    if (!spi3_owner || spi->Instance != SPI3) return;
    // The vendor SPI callback waits up to 100 ms for BSY in an IRQ. Replace it
    // with a fixed instruction budget; an abnormal bus goes to task recovery.
    unsigned remaining = 128;
    while (remaining && (spi->Instance->SR & SPI_SR_BSY)) { --remaining; __NOP(); }
    CLEAR_BIT(spi->Instance->CR2, SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
    if (!remaining || (spi->Instance->SR & (SPI_SR_OVR | SPI_SR_MODF | SPI_SR_FRE))
        || spi->hdmatx->ErrorCode != HAL_DMA_ERROR_NONE
        || spi->hdmarx->ErrorCode != HAL_DMA_ERROR_NONE
        || ((spi->hdmatx->Instance->CR | spi->hdmarx->Instance->CR) & DMA_SxCR_EN)) {
        spi3_owner->on_error(); return;
    }
    // RX completed last on the wire. A lower-priority TX IRQ may still be
    // pending; retire it before any next transaction can install callbacks.
    auto* tx = spi->hdmatx;
    __HAL_DMA_CLEAR_FLAG(tx, __HAL_DMA_GET_TC_FLAG_INDEX(tx) | __HAL_DMA_GET_HT_FLAG_INDEX(tx));
    NVIC_ClearPendingIRQ(dma_irq(tx->Instance));
    tx->State = HAL_DMA_STATE_READY; tx->Lock = HAL_UNLOCKED;
    spi->TxXferCount = spi->RxXferCount = 0;
    spi->State = HAL_SPI_STATE_READY;
    if (spi->ErrorCode != HAL_SPI_ERROR_NONE) spi3_owner->on_error();
    else spi3_owner->on_complete();
}
}

bool Stm32SpiArbiter::acquire_task(SpiTask* task) {
    return task && !__atomic_exchange_n(&task->is_in_use, true, __ATOMIC_ACQ_REL);
}
void Stm32SpiArbiter::release_task(SpiTask* task) {
    __atomic_store_n(&task->is_in_use, false, __ATOMIC_RELEASE);
}
Stm32SpiArbiter::Diagnostics Stm32SpiArbiter::diagnostics() const {
    CriticalSectionContext lock;
    return diagnostics_;
}

void Stm32SpiArbiter::transfer_async(SpiTask* task) {
    if (!task) return;
    bool reject = false;
    {
        CriticalSectionContext lock;
        // Re-enqueueing an already-owned descriptor must not invoke its callback
        // a second time: ownership remains with the original request.
        if (task == active_) { ++diagnostics_.rejected; return; }
        for (SpiTask* q = task_list_; q; q = q->next)
            if (q == task) { ++diagnostics_.rejected; return; }
        reject = diagnostics_.quarantined || queued_count_ >= 8 || !task->length
            || task->length > 260 || byte_count(*task) > sizeof(dma_tx_)
            || (!task->tx_buf && !task->rx_buf) || !task->ncs_gpio
            || (task->config.DataSize != SPI_DATASIZE_8BIT && task->config.DataSize != SPI_DATASIZE_16BIT);
        if (reject) {
            ++diagnostics_.rejected;
        } else {
            if (!task->timeout_ms || task->timeout_ms > 1000) task->timeout_ms = 1000;
            task->queued_ms = HAL_GetTick();
            SpiTask** q = &task_list_;
            while (*q && (*q)->priority <= task->priority) q = &(*q)->next;
            task->next = *q;
            *q = task;
            ++queued_count_;
            if (queued_count_ > diagnostics_.highwater) diagnostics_.highwater = queued_count_;
        }
    }
    if (reject) callback(task, false);
    else start();
}

bool Stm32SpiArbiter::start() {
    SpiTask* task;
    {
        CriticalSectionContext lock;
        if (phase_ != Phase::Idle || !task_list_ || diagnostics_.quarantined) return false;
        // SPI DMA IRQs outrank the motor control IRQ on this board. HAL MSP
        // reconfiguration includes GPIO/DMA setup and a tick-based DMA wait;
        // it must never execute in that interrupt context. Leave the queue
        // intact for service() or a thread caller to launch it safely.
        if (__get_IPSR() && (force_reinit_ || !same_config(task_list_->config, hspi_->Init))) return false;
        task = active_ = task_list_;
        task_list_ = task->next;
        --queued_count_;
        phase_ = Phase::Launching;
        pending_error_ = pending_completion_ = false;
    }
    // Launching owns both DMA buffers. service()/ISRs can only mark pending
    // events until HAL start returns; no cancel/start interleaving is possible.
    const size_t bytes = byte_count(*task);
    if (task->tx_buf) std::memcpy(dma_tx_, task->tx_buf, bytes);
    else std::memset(dma_tx_, 0xff, bytes);
    HAL_StatusTypeDef status = HAL_OK;
    if (force_reinit_ || !same_config(task->config, hspi_->Init)) {
        status = HAL_SPI_DeInit(hspi_);
        hspi_->Init = task->config;
        if (status == HAL_OK) status = HAL_SPI_Init(hspi_);
        if (status == HAL_OK) force_reinit_ = false;
    }
    if (status == HAL_OK && hspi_->hdmatx && hspi_->hdmarx
        && hspi_->hdmatx->State == HAL_DMA_STATE_READY && hspi_->hdmarx->State == HAL_DMA_STATE_READY) {
        const IRQn_Type tx_irq = dma_irq(hspi_->hdmatx->Instance);
        const IRQn_Type rx_irq = dma_irq(hspi_->hdmarx->Instance);
        const bool tx_enabled = NVIC->ISER[static_cast<uint32_t>(tx_irq) >> 5] & (1u << (static_cast<uint32_t>(tx_irq) & 31));
        const bool rx_enabled = NVIC->ISER[static_cast<uint32_t>(rx_irq) >> 5] & (1u << (static_cast<uint32_t>(rx_irq) & 31));
        NVIC_DisableIRQ(tx_irq); NVIC_DisableIRQ(rx_irq);
        __DSB(); __ISB();
        spi3_owner = this;
        task->ncs_gpio.write(false);
        // Full duplex even for write-only requests drains RX and avoids OVR.
        status = HAL_SPI_TransmitReceive_DMA(hspi_, dma_tx_, dma_rx_, task->length);
        hspi_->hdmarx->XferCpltCallback = dma_complete;
        hspi_->hdmarx->XferHalfCpltCallback = nullptr;
        hspi_->hdmarx->XferErrorCallback = dma_error;
        hspi_->hdmatx->XferCpltCallback = nullptr;
        hspi_->hdmatx->XferHalfCpltCallback = nullptr;
        hspi_->hdmatx->XferErrorCallback = dma_error;
        CLEAR_BIT(hspi_->hdmarx->Instance->CR, DMA_SxCR_HTIE);
        CLEAR_BIT(hspi_->hdmatx->Instance->CR, DMA_SxCR_HTIE);
        if (tx_enabled) NVIC_EnableIRQ(tx_irq);
        if (rx_enabled) NVIC_EnableIRQ(rx_irq);
    } else {
        status = HAL_ERROR;
    }
    bool failed, completed;
    {
        CriticalSectionContext lock;
        failed = status != HAL_OK || pending_error_;
        completed = pending_completion_;
        phase_ = failed ? Phase::Aborting : Phase::Running;
        if (failed) ++diagnostics_.errors;
    }
    if (failed) {
        if (!quiesce()) { CriticalSectionContext lock; diagnostics_.quarantined = true; }
        finish(false);
    } else if (completed) {
        finish(true);
    }
    return !failed;
}

bool Stm32SpiArbiter::quiesce() {
    // Only SPI3/DMA1 is supported by this board. Refuse to operate an unexpected
    // peripheral instead of disabling an unrelated interrupt.
    if (hspi_->Instance != SPI3 || !hspi_->hdmatx || !hspi_->hdmarx) return false;
    DMA_HandleTypeDef* streams[] = { hspi_->hdmatx, hspi_->hdmarx };
    const IRQn_Type irqs[] = { dma_irq(streams[0]->Instance), dma_irq(streams[1]->Instance), SPI3_IRQn };
    bool enabled[3];
    for (unsigned i = 0; i < 3; ++i) {
        enabled[i] = (NVIC->ISER[static_cast<uint32_t>(irqs[i]) >> 5]
            & (1u << (static_cast<uint32_t>(irqs[i]) & 31))) != 0;
        NVIC_DisableIRQ(irqs[i]);
    }
    __DSB(); __ISB();
    CLEAR_BIT(hspi_->Instance->CR2, SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN | SPI_CR2_ERRIE | SPI_CR2_RXNEIE | SPI_CR2_TXEIE);
    __HAL_SPI_DISABLE(hspi_);
    for (auto* dma : streams) {
        CLEAR_BIT(dma->Instance->CR, DMA_SxCR_EN | DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE | DMA_SxCR_DMEIE);
        CLEAR_BIT(dma->Instance->FCR, DMA_SxFCR_FEIE);
    }
    // Bounded hardware drain, with all unrelated/control interrupts enabled.
    // No HAL Abort loop and no waiting on SysTick from an interrupt context.
    unsigned remaining = 1024;
    while (remaining-- && ((streams[0]->Instance->CR | streams[1]->Instance->CR) & DMA_SxCR_EN)) __NOP();
    const bool stopped = !((streams[0]->Instance->CR | streams[1]->Instance->CR) & DMA_SxCR_EN);
    for (auto* dma : streams) {
        __HAL_DMA_CLEAR_FLAG(dma, __HAL_DMA_GET_TC_FLAG_INDEX(dma) | __HAL_DMA_GET_HT_FLAG_INDEX(dma)
            | __HAL_DMA_GET_TE_FLAG_INDEX(dma) | __HAL_DMA_GET_DME_FLAG_INDEX(dma) | __HAL_DMA_GET_FE_FLAG_INDEX(dma));
        if (stopped) { dma->State = HAL_DMA_STATE_READY; dma->ErrorCode = HAL_DMA_ERROR_NONE; dma->Lock = HAL_UNLOCKED; }
    }
    __HAL_SPI_CLEAR_OVRFLAG(hspi_);
    if (stopped) {
        hspi_->State = HAL_SPI_STATE_READY;
        hspi_->ErrorCode = HAL_SPI_ERROR_NONE;
        hspi_->Lock = HAL_UNLOCKED;
        force_reinit_ = true; // restore SPI mode bits after OVR/MODF/bus faults
    }
    for (unsigned i = 0; i < 3; ++i) {
        NVIC_ClearPendingIRQ(irqs[i]);
        if (enabled[i] && stopped) NVIC_EnableIRQ(irqs[i]);
    }
    __DSB(); __ISB();
    return stopped;
}

void Stm32SpiArbiter::finish(bool success) {
    SpiTask* task;
    {
        CriticalSectionContext lock;
        if (!active_ || (phase_ != Phase::Running && phase_ != Phase::Aborting)) return;
        phase_ = Phase::Completing;
        task = active_;
        active_ = nullptr;
    }
    task->ncs_gpio.write(true);
    if (success && task->rx_buf) std::memcpy(task->rx_buf, dma_rx_, byte_count(*task));
    callback(task, success);
    // Never access task after callback. Enqueues from callback see Completing.
    {
        CriticalSectionContext lock;
        phase_ = Phase::Idle;
    }
    start();
}

void Stm32SpiArbiter::on_complete() {
    {
        CriticalSectionContext lock;
        if (phase_ == Phase::Launching) { pending_completion_ = true; return; }
        if (phase_ != Phase::Running || pending_error_) return;
    }
    finish(true);
}
void Stm32SpiArbiter::on_error() {
    CriticalSectionContext lock;
    if (phase_ == Phase::Running || phase_ == Phase::Launching) pending_error_ = true;
}

void Stm32SpiArbiter::service(uint32_t now_ms) {
    // At most eight detached queue failures per service; callbacks are outside
    // the list lock and may safely enqueue a new descriptor.
    SpiTask* failures[8];
    unsigned count = 0;
    bool abort = false;
    {
        CriticalSectionContext lock;
        SpiTask** q = &task_list_;
        while (*q) {
            SpiTask* t = *q;
            if (diagnostics_.quarantined || static_cast<uint32_t>(now_ms - t->queued_ms) >= t->timeout_ms) {
                *q = t->next;
                --queued_count_;
                failures[count++] = t;
                ++diagnostics_.timeouts;
            } else q = &t->next;
        }
        if (active_ && (phase_ == Phase::Launching || phase_ == Phase::Running)) {
            if (static_cast<uint32_t>(now_ms - active_->queued_ms) >= active_->timeout_ms && !pending_error_) {
                pending_error_ = true;
                ++diagnostics_.timeouts;
            }
            if (pending_error_ && phase_ == Phase::Running) {
                phase_ = Phase::Aborting;
                abort = true;
                ++diagnostics_.errors;
            }
        }
    }
    for (unsigned i = 0; i < count; ++i) callback(failures[i], false);
    if (abort) {
        if (!quiesce()) { CriticalSectionContext lock; diagnostics_.quarantined = true; }
        finish(false);
    }
    start();
}

bool Stm32SpiArbiter::transfer(SPI_InitTypeDef config, Stm32Gpio cs, const uint8_t* tx,
        uint8_t* rx, size_t length, uint32_t timeout_ms) {
    return transfer_priority(config, cs, tx, rx, length, timeout_ms, 0);
}
bool Stm32SpiArbiter::transfer_priority(SPI_InitTypeDef config, Stm32Gpio cs, const uint8_t* tx,
        uint8_t* rx, size_t length, uint32_t timeout_ms, uint8_t priority) {
    // Blocking calls from an IRQ could prevent the completion IRQ indefinitely.
    if (__get_IPSR() || __get_PRIMASK()) return false;
    uint8_t result = 0xff;
    SpiTask task{};
    task.config = config; task.ncs_gpio = cs; task.tx_buf = tx; task.rx_buf = rx; task.length = length;
    task.timeout_ms = timeout_ms ? timeout_ms : 1; task.priority = priority;
    task.on_complete = [](void* ctx, bool ok) { __atomic_store_n(static_cast<uint8_t*>(ctx), ok ? 1 : 0, __ATOMIC_RELEASE); };
    task.on_complete_ctx = &result;
    transfer_async(&task);
    while (__atomic_load_n(&result, __ATOMIC_ACQUIRE) == 0xff) {
        service(HAL_GetTick());
        if (__atomic_load_n(&result, __ATOMIC_ACQUIRE) == 0xff) osDelay(1);
    }
    return result == 1;
}
