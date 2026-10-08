#pragma once
#include <cstdint>
#include <cstddef>
struct SPI_InitTypeDef {
    uint32_t Mode = 0, Direction = 0, DataSize = 0, CLKPolarity = 0, CLKPhase = 0;
    uint32_t NSS = 0, BaudRatePrescaler = 0, FirstBit = 0, TIMode = 0, CRCCalculation = 0, CRCPolynomial = 0;
};
struct DMA_Stream_TypeDef { uint32_t CR = 0, FCR = 0; };
struct DMA_HandleTypeDef {
    DMA_Stream_TypeDef* Instance = nullptr;
    int State = 0, Lock = 0; uint32_t ErrorCode = 0;
    void* Parent = nullptr;
    void (*XferCpltCallback)(DMA_HandleTypeDef*) = nullptr;
    void (*XferHalfCpltCallback)(DMA_HandleTypeDef*) = nullptr;
    void (*XferErrorCallback)(DMA_HandleTypeDef*) = nullptr;
};
struct SPI_TypeDef { uint32_t CR2 = 0, SR = 0; };
struct SPI_HandleTypeDef {
    SPI_TypeDef* Instance = nullptr; SPI_InitTypeDef Init{};
    DMA_HandleTypeDef *hdmatx = nullptr, *hdmarx = nullptr;
    int State = 0, Lock = 0; uint32_t ErrorCode = 0;
    uint16_t TxXferCount = 0, RxXferCount = 0;
};
extern SPI_TypeDef mock_spi3;
extern DMA_Stream_TypeDef mock_streams[8];
#define SPI3 (&mock_spi3)
#define DMA1_Stream0 (&mock_streams[0])
#define DMA1_Stream1 (&mock_streams[1])
#define DMA1_Stream2 (&mock_streams[2])
#define DMA1_Stream3 (&mock_streams[3])
#define DMA1_Stream4 (&mock_streams[4])
#define DMA1_Stream5 (&mock_streams[5])
#define DMA1_Stream6 (&mock_streams[6])
#define DMA1_Stream7 (&mock_streams[7])
enum IRQn_Type { DMA1_Stream0_IRQn = 10, DMA1_Stream1_IRQn, DMA1_Stream2_IRQn,
    DMA1_Stream3_IRQn, DMA1_Stream4_IRQn, DMA1_Stream5_IRQn, DMA1_Stream6_IRQn,
    DMA1_Stream7_IRQn, SPI3_IRQn };
enum HAL_StatusTypeDef { HAL_OK, HAL_ERROR, HAL_BUSY };
#define SPI_MODE_MASTER 1
#define SPI_DIRECTION_2LINES 1
#define SPI_DATASIZE_8BIT 0
#define SPI_DATASIZE_16BIT 1
#define SPI_POLARITY_LOW 0
#define SPI_PHASE_1EDGE 0
#define SPI_PHASE_2EDGE 1
#define SPI_NSS_SOFT 1
#define SPI_BAUDRATEPRESCALER_16 16
#define SPI_FIRSTBIT_MSB 0
#define SPI_TIMODE_DISABLE 0
#define SPI_CRCCALCULATION_DISABLE 0
#define HAL_DMA_STATE_READY 0
#define HAL_SPI_STATE_READY 0
#define HAL_DMA_ERROR_NONE 0
#define HAL_SPI_ERROR_NONE 0
#define HAL_UNLOCKED 0
#define SPI_CR2_TXDMAEN 1u
#define SPI_CR2_RXDMAEN 2u
#define SPI_CR2_ERRIE 4u
#define SPI_CR2_RXNEIE 8u
#define SPI_CR2_TXEIE 16u
#define SPI_SR_BSY 1u
#define SPI_SR_OVR 2u
#define SPI_SR_MODF 4u
#define SPI_SR_FRE 8u
#define DMA_SxCR_EN 1u
#define DMA_SxCR_TCIE 2u
#define DMA_SxCR_HTIE 4u
#define DMA_SxCR_TEIE 8u
#define DMA_SxCR_DMEIE 16u
#define DMA_SxFCR_FEIE 1u
#define CLEAR_BIT(reg, bits) ((reg) &= ~(bits))
#define __HAL_SPI_DISABLE(h) ((void)(h))
#define __HAL_SPI_CLEAR_OVRFLAG(h) ((void)(h))
#define __HAL_DMA_GET_TC_FLAG_INDEX(h) 1u
#define __HAL_DMA_GET_HT_FLAG_INDEX(h) 2u
#define __HAL_DMA_GET_TE_FLAG_INDEX(h) 4u
#define __HAL_DMA_GET_DME_FLAG_INDEX(h) 8u
#define __HAL_DMA_GET_FE_FLAG_INDEX(h) 16u
extern uint32_t mock_flags_cleared;
#define __HAL_DMA_CLEAR_FLAG(h, flags) (mock_flags_cleared |= (flags))
uint32_t HAL_GetTick();
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef*);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef*);
HAL_StatusTypeDef HAL_SPI_TransmitReceive_DMA(SPI_HandleTypeDef*, uint8_t*, uint8_t*, uint16_t);
