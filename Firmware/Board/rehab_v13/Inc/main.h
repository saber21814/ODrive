/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * This notice applies to any and all portions of this file
  * that are not between comment pairs USER CODE BEGIN and
  * USER CODE END. Other portions of this file, whether 
  * inserted by the user or by software development tools
  * are owned by their respective copyright owners.
  *
  * Copyright (c) 2018 STMicroelectronics International N.V. 
  * All rights reserved.
  *
  * Redistribution and use in source and binary forms, with or without 
  * modification, are permitted, provided that the following conditions are met:
  *
  * 1. Redistribution of source code must retain the above copyright notice, 
  *    this list of conditions and the following disclaimer.
  * 2. Redistributions in binary form must reproduce the above copyright notice,
  *    this list of conditions and the following disclaimer in the documentation
  *    and/or other materials provided with the distribution.
  * 3. Neither the name of STMicroelectronics nor the names of other 
  *    contributors to this software may be used to endorse or promote products 
  *    derived from this software without specific written permission.
  * 4. This software, including modifications and/or derivative works of this 
  *    software, must execute solely and exclusively on microcontroller or
  *    microprocessor devices manufactured by or for STMicroelectronics.
  * 5. Redistribution and use of this software other than as permitted under 
  *    this license is void and will automatically terminate your rights under 
  *    this license. 
  *
  * THIS SOFTWARE IS PROVIDED BY STMICROELECTRONICS AND CONTRIBUTORS "AS IS" 
  * AND ANY EXPRESS, IMPLIED OR STATUTORY WARRANTIES, INCLUDING, BUT NOT 
  * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
  * PARTICULAR PURPOSE AND NON-INFRINGEMENT OF THIRD PARTY INTELLECTUAL PROPERTY
  * RIGHTS ARE DISCLAIMED TO THE FULLEST EXTENT PERMITTED BY LAW. IN NO EVENT 
  * SHALL STMICROELECTRONICS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
  * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
  * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
  * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
  * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  *
  ******************************************************************************
  */

#ifndef REHAB_V13_MAIN_H
#define REHAB_V13_MAIN_H
#ifndef REHAB_V12
#define REHAB_V12 1
#endif
#ifndef REHAB_V13
#define REHAB_V13 1
#endif
#include "stm32f4xx_hal.h"
#define TIM_1_8_CLOCK_HZ 168000000
#define TIM_1_8_PERIOD_CLOCKS 3500
#define TIM_1_8_DEADTIME_CLOCKS 20
#define TIM_APB1_CLOCK_HZ 84000000
#define TIM_APB1_PERIOD_CLOCKS 4096
#define TIM_APB1_DEADTIME_CLOCKS 40
#define TIM_1_8_RCR 2

#define M0_nCS_Pin GPIO_PIN_13
#define M0_nCS_GPIO_Port GPIOC
#define M0_IB_Pin GPIO_PIN_0
#define M0_IB_GPIO_Port GPIOC
#define M0_IC_Pin GPIO_PIN_1
#define M0_IC_GPIO_Port GPIOC
#define GPIO_1_Pin GPIO_PIN_0
#define GPIO_1_GPIO_Port GPIOA
#define GPIO_2_Pin GPIO_PIN_1
#define GPIO_2_GPIO_Port GPIOA
#define GPIO_3_Pin GPIO_PIN_2
#define GPIO_3_GPIO_Port GPIOA
#define GPIO_4_Pin GPIO_PIN_3
#define GPIO_4_GPIO_Port GPIOA
#define AUX_TEMP_Pin GPIO_PIN_0
#define AUX_TEMP_GPIO_Port GPIOB
#define VBUS_S_Pin GPIO_PIN_1
#define VBUS_S_GPIO_Port GPIOA
#define GPIO_5_Pin GPIO_PIN_4
#define GPIO_5_GPIO_Port GPIOC
#define M0_TEMP_Pin GPIO_PIN_5
#define M0_TEMP_GPIO_Port GPIOC
#define GPIO_6_Pin GPIO_PIN_2
#define GPIO_6_GPIO_Port GPIOB
#define AUX_L_Pin GPIO_PIN_10
#define AUX_L_GPIO_Port GPIOB
#define AUX_H_Pin GPIO_PIN_11
#define AUX_H_GPIO_Port GPIOB
#define GATE_Pin GPIO_PIN_12
#define GATE_GPIO_Port GPIOB
// Firmware controls GATE, the input to the STOP AND gate; EN_GATE is its output.
#define EN_GATE_Pin GATE_Pin
#define EN_GATE_GPIO_Port GPIOB
#define M0_AL_Pin GPIO_PIN_13
#define M0_AL_GPIO_Port GPIOB
#define M0_BL_Pin GPIO_PIN_14
#define M0_BL_GPIO_Port GPIOB
#define M0_CL_Pin GPIO_PIN_15
#define M0_CL_GPIO_Port GPIOB
#define M0_ENC_Z_Pin GPIO_PIN_9
#define M0_ENC_Z_GPIO_Port GPIOC
#define M0_AH_Pin GPIO_PIN_8
#define M0_AH_GPIO_Port GPIOA
#define M0_BH_Pin GPIO_PIN_9
#define M0_BH_GPIO_Port GPIOA
#define M0_CH_Pin GPIO_PIN_10
#define M0_CH_GPIO_Port GPIOA
#define GPIO_7_Pin GPIO_PIN_15
#define GPIO_7_GPIO_Port GPIOA
#define nFAULT_Pin GPIO_PIN_2
#define nFAULT_GPIO_Port GPIOD
#define GPIO_8_Pin GPIO_PIN_3
#define GPIO_8_GPIO_Port GPIOB
#define M0_ENC_A_Pin GPIO_PIN_4
#define M0_ENC_A_GPIO_Port GPIOB
#define M0_ENC_B_Pin GPIO_PIN_5
#define M0_ENC_B_GPIO_Port GPIOB

#define CH394Q_CS_Pin GPIO_PIN_2
#define CH394Q_CS_GPIO_Port GPIOC
#define CH394Q_RESET_Pin GPIO_PIN_4
#define CH394Q_RESET_GPIO_Port GPIOA
#define CH394Q_INT_Pin GPIO_PIN_8
#define CH394Q_INT_GPIO_Port GPIOC
#define HX711_DOUT_Pin GPIO_PIN_6
#define HX711_DOUT_GPIO_Port GPIOC
#define HX711_SCK_Pin GPIO_PIN_7
#define HX711_SCK_GPIO_Port GPIOC
#define HX711_RATE_Pin GPIO_PIN_1
#define HX711_RATE_GPIO_Port GPIOB
#define STOP_Pin GPIO_PIN_4
#define STOP_GPIO_Port GPIOC
#define DC_CAL_Pin GPIO_PIN_7
#define DC_CAL_GPIO_Port GPIOB

#ifdef __cplusplus
extern "C" {
#endif
void _Error_Handler(char *, int);
#define Error_Handler() _Error_Handler(__FILE__, __LINE__)
#ifdef __cplusplus
}
#endif
#endif
