/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (Rain mm integer display)
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* USER CODE BEGIN Includes */
#include "i2c-lcd.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NORMAL_RAIN_MM    15.0f    // 보통 비(mm)
#define WARNING_RAIN_MM   34.0f    // 폭우 경고(mm)
#define SENSOR_MAX_MM     40.0f    // 빨간 수위센서 측정 최대 높이(mm)
#define IR_PIN GPIO_PIN_8
#define IR_PORT GPIOA
/* USER CODE END PD */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;
TIM_HandleTypeDef htim3;
UART_HandleTypeDef huart2;
TIM_HandleTypeDef htim4;
/* FreeRTOS thread handles */
osThreadId_t lcdTaskHandle;
osThreadId_t servoTaskHandle;

/* USER CODE BEGIN PV */
float rain_mm = 0.0f;
float smooth_rain_mm = 0.0f;
char line1[17];
char line2[17];
int flood_counter = 0;
volatile uint8_t barrier_up = 0;    // 1 if barrier is raised, 0 if lowered
volatile uint8_t manual_mode = 0;   // 1 if IR manual override is active, 0 if in automatic mode
volatile uint8_t ir_ready = 0;
volatile uint32_t ir_timings[100];
volatile uint8_t ir_index = 0;
volatile uint32_t last_edge_time = 0;
volatile uint32_t last_ir_code = 0;
/* USER CODE END PV */

/* Function prototypes -------------------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART2_UART_Init(void);
void StartLcdTask(void *argument);
void StartWaterTask(void *argument);
void StartIrTask(void *argument);
uint32_t decode_nec_signal(void);
void set_servo_angle(uint8_t angle);
float read_rain_mm(void);
float read_smoothed_rain_mm(void);
void lcd_display_rain(const char* status);
/* USER CODE BEGIN 0 */
uint32_t decode_nec_signal()
{
    // NEC protocol requires ~68 timing edges (including leader and spaces)
       if (ir_index < 68 || ir_index > 100) {
           return 0xFFFFFFFF;
       }
       uint32_t data = 0;
       for (int i = 0; i < 32; i++) {
           uint32_t t = ir_timings[17 + i];
           // Logical 1: typically ~1.6-2.4 ms high pulse
           if (t > 1400) {
               data |= (1UL << (31 - i));
           }
           // Logical 0: typically ~0.5 ms high pulse
           else if (t > 200 && t <= 1400) {
               // bit remains 0 (do nothing)
           }
           // Timing out of range: error
           else {
               return 0xFFFFFFFF;
           }
       }
       return data;
}
/* 서보 각도 제어 */

void set_servo_angle(uint8_t angle) {
    uint32_t pulse = ((angle * 2000) / 180) + 500; // Map 0-180° to 500-2500us pulse
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, pulse);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, pulse);
}

/* 센서 값 읽고 mm 변환 */

float read_rain_mm() {
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, HAL_MAX_DELAY);
    uint16_t raw = HAL_ADC_GetValue(&hadc1);
    if (raw > 4000) raw = 4000;  // clamp to max expected ADC value
    return (raw / 4095.0f) * SENSOR_MAX_MM;
}
float read_smoothed_rain_mm() {
    float current = read_rain_mm();
    // 80% previous value + 20% new value for smoothing
    smooth_rain_mm = (smooth_rain_mm * 0.8f) + (current * 0.2f);
    return smooth_rain_mm;
}
/* LCD에 강수량 표시 (정수 버전) */
void lcd_display_rain(const char* status) {
    int rain_int = (int)rain_mm; // truncate to integer mm
    sprintf(line1, "Rain: %3d mm", rain_int);
    sprintf(line2, "Status: %s", status);
    lcd_put_cur(0, 0);
    lcd_send_string(line1);
    lcd_put_cur(1, 0);
    lcd_send_string(line2);
}

/* LCD Task */
void StartLcdTask(void *argument) {
  /* USER CODE BEGIN StartLcdTask */
  for (;;) {
    rain_mm = read_smoothed_rain_mm();
    if (rain_mm < NORMAL_RAIN_MM) {
        lcd_display_rain("NORMAL");
    } else if (rain_mm < WARNING_RAIN_MM) {
        lcd_display_rain("WARNING");
    } else {
        lcd_display_rain("!!FLOOD!!");
    }
    osDelay(1000);
  }
  /* USER CODE END StartLcdTask */
}

/* Servo Task */
void StartWaterTask(void *argument) {
  /* USER CODE BEGIN StartWaterTask */
  for (;;) {
    rain_mm = read_smoothed_rain_mm();

    if (!manual_mode) {
      // Automatic control active only when not in manual override
      if (rain_mm >= WARNING_RAIN_MM) {
        // Heavy rain: raise barrier if not already up
        if (!barrier_up) {
          set_servo_angle(90);           // raise barrier
          barrier_up = 1;
          // Activate flood indicators
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);   // Red LED ON
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_SET);   // Buzzer ON
        }
      } else if (rain_mm < NORMAL_RAIN_MM) {
        // Light rain/normal: lower barrier if it was up
        if (barrier_up) {
          set_servo_angle(0);            // lower barrier
          barrier_up = 0;
          // Deactivate flood indicators
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_RESET); // Red LED OFF
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_RESET); // Buzzer OFF
        }
      }
      // (If rain is in between NORMAL_RAIN_MM and WARNING_RAIN_MM, barrier state remains as is.)
    }

    // Status LEDs for rain level (Green/Yellow).
    // If in manual mode with barrier up, override status LEDs to off (red LED indicates manual override/heavy state).
    if (manual_mode && barrier_up) {
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET); // Green OFF
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET); // Yellow OFF
    } else {
      if (rain_mm < NORMAL_RAIN_MM) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_SET);    // Green ON (safe)
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET);  // Yellow OFF
      } else if (rain_mm < WARNING_RAIN_MM) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);  // Green OFF
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_SET);    // Yellow ON (warning)
      } else {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET);  // Green OFF
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET);  // Yellow OFF (flood, red is handled above)
      }
    }

    osDelay(100);  // small delay for loop
  }
  /* USER CODE END StartWaterTask */
}

void StartIrTask(void *argument) {
  /* USER CODE BEGIN StartIrTask */
  for (;;) {
    if (ir_ready) {
      // An IR code has been captured by the interrupt
      ir_ready = 0;
      uint32_t code = decode_nec_signal();
      ir_index = 0;
      if (code != 0xFFFFFFFF && code != last_ir_code) {
        // New valid IR code received (filter out repeats)
        last_ir_code = code;
        if (!manual_mode) {
          manual_mode = 1;  // enter manual mode on first IR command
        }
        // Toggle barrier state on IR command
        if (!barrier_up) {
          // Barrier is currently down -> raise it via remote
          set_servo_angle(90);
          barrier_up = 1;
          // Turn off green/yellow, turn on red LED and buzzer for manual raise
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0, GPIO_PIN_RESET); // Green OFF
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET); // Yellow OFF
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_SET);   // Red ON
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_SET);   // Buzzer ON
        } else {
          // Barrier is currently up -> lower it via remote
          set_servo_angle(0);
          barrier_up = 0;
          // Turn off red LED and buzzer for manual lower
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_2, GPIO_PIN_RESET); // Red OFF
          HAL_GPIO_WritePin(GPIOC, GPIO_PIN_3, GPIO_PIN_RESET); // Buzzer OFF
        }
        // If barrier has been lowered by remote, exit manual mode (return to automatic)
        if (barrier_up == 0) {
          manual_mode = 0;
        }
      }
    }
    osDelay(50);  // short polling delay
  }
  /* USER CODE END StartIrTask */
}
/* USER CODE END 0 */

/* Main function */
int main(void) {
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_ADC1_Init();
    MX_I2C1_Init();
    MX_TIM3_Init();
    MX_TIM4_Init();
    MX_USART2_UART_Init();

    HAL_TIM_Base_Start(&htim4);

    lcd_init();
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);

    osKernelInitialize();
    lcdTaskHandle   = osThreadNew(StartLcdTask, NULL, NULL);  // LCD task
    servoTaskHandle = osThreadNew(StartWaterTask, NULL, NULL); // 센서(자동) task
    osThreadNew(StartIrTask, NULL, NULL); // IR(수동) task 따로 등록
    osKernelStart();

    while (1) {}
}

  /* USER CODE END 3 */


/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_4;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 83;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 20000;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */
  HAL_TIM_MspPostInit(&htim3);

}

static void MX_TIM4_Init(void)
{
    __HAL_RCC_TIM4_CLK_ENABLE();

    htim4.Instance = TIM4;
    htim4.Init.Prescaler = 83;
    htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim4.Init.Period = 0xFFFF;
    htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim4) != HAL_OK)
    {
        Error_Handler();
    }
}
/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : PC0 PC1 PC2 PC3 */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* Configure IR Receiver Pin (PA8) as EXTI */
  GPIO_InitStruct.Pin = IR_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;  // 보통 IR 수신기 출력은 오픈컬렉터
  HAL_GPIO_Init(IR_PORT, &GPIO_InitStruct);


  HAL_NVIC_SetPriority(EXTI9_5_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
  /* USER CODE BEGIN HAL_GPIO_EXTI_Callback */
  if (GPIO_Pin == IR_PIN) {
    // IR receiver falling edge interrupt
    uint32_t now = __HAL_TIM_GET_COUNTER(&htim4);
    uint32_t duration = (now >= last_edge_time) ? (now - last_edge_time)
                                               : (0xFFFF - last_edge_time + now);
    last_edge_time = now;
    if (ir_index < 100) {
      ir_timings[ir_index++] = duration;
    }
    // If a full IR code (≈68 edges) is captured, signal readiness to decode
    if (ir_index >= 68) {
      ir_ready = 1;
    }
  }
  /* USER CODE END HAL_GPIO_EXTI_Callback */
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartTask02 */
/**
* @brief Function implementing the LcdTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTask02 */
void StartTask02(void *argument)
{
  /* USER CODE BEGIN StartTask02 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartTask02 */
}

/* USER CODE BEGIN Header_StartTask03 */
/**
* @brief Function implementing the ServoTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartTask03 */
void StartTask03(void *argument)
{
  /* USER CODE BEGIN StartTask03 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartTask03 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM9 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM9)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
