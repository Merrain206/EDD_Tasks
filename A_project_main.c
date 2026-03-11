/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

typedef enum {
    MODE_0_ON,      // 常亮
    MODE_1_OFF,     // 熄灭
    MODE_2_BLINK,   // 闪烁
    MODE_3_BREATH,  // 呼吸
    MODE_4_ENCODER  // 编码器
} LED_Mode_t;

LED_Mode_t currentMode = MODE_0_ON; // 默认上电执行 Mode0
uint16_t pwm_duty = 0;              // 当前占空比 (0-1000)
uint32_t last_tick = 0;             // 用于 Mode 2 闪烁计时
uint32_t breath_last_tick = 0; 		// 记录呼吸灯上一次更新的时间
int8_t breath_dir = 1;         		// 呼吸方向：1代表逐渐变亮，-1代表逐渐变暗

//拓展：按键检测，消除卡顿
uint8_t button_state = 0;       // 按键状态：0-未按下，1-消抖中，2-等待松开
uint32_t button_tick = 0;       // 记录按键按下的时间戳

// === 串口接收变量 ===
uint8_t rx_data;          // 每次中断接收的一个字节
char rx_buffer[20];       // 接收字符串缓冲区
uint8_t rx_index = 0;     // 缓冲区当前索引
uint8_t rx_flag = 0;      // 接收完成标志位 (收到 \n 置 1)

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

// 定义一个安全的 Flash 页面地址 (使用 Page 62)
#define FLASH_SAVE_ADDR 0x0800F800

// 写入模式到 Flash 的函数
void Save_Mode_To_Flash(uint32_t mode) {
    HAL_FLASH_Unlock(); // 1. 解锁 Flash

    // 2. 配置擦除参数并擦除这一页
    FLASH_EraseInitTypeDef EraseInitStruct;
    uint32_t PageError;
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.PageAddress = FLASH_SAVE_ADDR;
    EraseInitStruct.NbPages = 1;
    HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);

    // 3. 将模式数据写入 Flash (以 Word/32位 格式)
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, FLASH_SAVE_ADDR, mode);

    HAL_FLASH_Lock();   // 4. 重新上锁保护
}

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  // === 上电读取 Flash 断电记忆 ===
    uint32_t saved_mode = *(__IO uint32_t*)FLASH_SAVE_ADDR; // 直接通过指针读取物理地址

    if (saved_mode <= MODE_4_ENCODER) {
        currentMode = (LED_Mode_t)saved_mode; // 如果数据合法，恢复模式
    } else {
        currentMode = MODE_0_ON;              // 如果是第一次运行或数据非法，默认模式 0
    }

  OLED_Init(&hi2c1);
  OLED_Clear(&hi2c1);

  // 启动定时器 PWM 输出
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);

  // 启动定时器编码器模式
  HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);

  // 开启串口接收中断 (每次接收 1 个字节)
  HAL_UART_Receive_IT(&huart1, &rx_data, 1);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
    while (1)
    {
    // ================= 1. 按键扫描与模式切换 (非阻塞状态机) =================
    uint8_t current_btn = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1); // 读取当前按键引脚电平

	  switch (button_state) {
		  case 0: // 状态 0：空闲等待按下
			  if (current_btn == GPIO_PIN_RESET) {
				  button_state = 1;               // 转移到消抖状态
				  button_tick = HAL_GetTick();    // 记录刚按下的时间
			  }
			  break;

		  case 1: // 状态 1：软件延时消抖
			  // 检查距离刚按下是否已经过了 20ms (替代 HAL_Delay)
			  if (HAL_GetTick() - button_tick >= 20) {
				  if (current_btn == GPIO_PIN_RESET) {
					  // 20ms后依然是按下状态，确认为有效按键触发

					  // 模式顺序切换
					  currentMode++;
					  if (currentMode > MODE_4_ENCODER) {
						  currentMode = MODE_0_ON;
					  }

					  // 如果是切换到了 Mode 4，同步当前亮度给编码器计数器
					  if (currentMode == MODE_4_ENCODER) {
						  __HAL_TIM_SET_COUNTER(&htim2, pwm_duty);
					  }

					  button_state = 2; // 触发动作完毕，转移到等待松开状态
				  } else {
					  // 可能是干扰毛刺，回到空闲状态
					  button_state = 0;
				  }
			  }
		  break;

		  case 2: // 状态 2：等待按键松开 (替代 while 死等)
			  if (current_btn == GPIO_PIN_SET) {  // 如果按键已经是高电平(松开)
				  button_state = 0;               // 回到空闲状态，准备迎接下一次按键
			  	  }
		  break;
	  }

      // ================= 2. 模式执行逻辑 =================
      switch (currentMode) {
          case MODE_0_ON:
              pwm_duty = 1000; // 占空比 100%，常亮
              __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm_duty);
              break;

          case MODE_1_OFF:
              pwm_duty = 0;    // 占空比 0%，熄灭
              __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm_duty);
              break;

          case MODE_2_BLINK:
              // 利用 SysTick 实现 1s 周期，亮灭各 500ms，不阻塞程序
              if (HAL_GetTick() - last_tick >= 500) {
                  last_tick = HAL_GetTick();
                  if (pwm_duty == 0) pwm_duty = 1000;
                  else pwm_duty = 0;
                  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm_duty);
              }
              break;

          case MODE_3_BREATH:
			  // 每 15ms 调整一次占空比，保证平滑不突变
			  if (HAL_GetTick() - breath_last_tick >= 15) {
				  breath_last_tick = HAL_GetTick();

				  if (breath_dir == 1) { // 变亮阶段
					  pwm_duty += 10;
					  if (pwm_duty >= 1000) {
						  pwm_duty = 1000;
						  breath_dir = -1; // 到达最亮，翻转方向为变暗
					  }
				  } else {               // 变暗阶段
					  if (pwm_duty <= 10) { // 防止无符号整数减法溢出
						  pwm_duty = 0;
						  breath_dir = 1;  // 到达最暗，翻转方向为变亮
					  } else {
						  pwm_duty -= 10;
					  }
				  }
				  // 更新 PWM 输出
				  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm_duty);
			  }
              break;

          case MODE_4_ENCODER:
        	  // 读取硬件编码器的计数值
			  int32_t encoder_val = __HAL_TIM_GET_COUNTER(&htim2);

			  // 限制范围在 0 ~ 1000 之间（对应 PWM 占空比的 0% ~ 100%）
			  if (encoder_val > 1000) {
				  // 如果超过 1000，说明可能是往回转发生了下溢出（比如从 0 减到 65535）
				  // 或者是往上转超过了 1000
				  if (encoder_val > 32768) {
					  encoder_val = 0; // 下溢出，限制为 0
				  } else {
					  encoder_val = 1000; // 上溢出，限制为 1000
				  }
				  // 强制写回计数器，防止一直在错误数值区间
				  __HAL_TIM_SET_COUNTER(&htim2, encoder_val);
			  }

			  // 更新占空比变量并输出
			  if (pwm_duty != encoder_val) {
				  pwm_duty = encoder_val;
				  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, pwm_duty);
			  }
              break;
      }

      // ================= 3. OLED 屏幕刷新 =================
      // 显示当前模式和占空比
      OLED_SetCursor(&hi2c1, 0, 0);
      OLED_WriteString(&hi2c1, "Mode: ");
      OLED_ShowNumber(&hi2c1, currentMode, 40, 0);

      OLED_SetCursor(&hi2c1, 0, 2);
      OLED_WriteString(&hi2c1, "Duty: ");
      OLED_ShowNumber(&hi2c1, pwm_duty / 10, 40, 2); // 换算成 0-100% 显示
      OLED_WriteString(&hi2c1, "%   "); // 留几个空格覆盖旧数据

      // ================= 4. 串口指令解析 =================
		if (rx_flag == 1) {
			// 处理不带记忆的临时切换指令 (@ModeX)
			if (strncmp(rx_buffer, "@Mode", 5) == 0) {
				uint8_t m = rx_buffer[5] - '0'; // 将字符 '0'-'4' 转换为数字 0-4
				if (m <= 4) {
					currentMode = (LED_Mode_t)m;
					if (currentMode == MODE_4_ENCODER) __HAL_TIM_SET_COUNTER(&htim2, pwm_duty);

					// 回传 ModeX_OK\r\n
					char reply[15];
					sprintf(reply, "Mode%d_OK\r\n", m);
					HAL_UART_Transmit(&huart1, (uint8_t *)reply, strlen(reply), 100);
				}
			}
			// 处理带记忆的保存指令 (@SaveMX)
			else if (strncmp(rx_buffer, "@SaveM", 6) == 0) {
				uint8_t m = rx_buffer[6] - '0';
				if (m <= 4) {
					currentMode = (LED_Mode_t)m;
					if (currentMode == MODE_4_ENCODER) __HAL_TIM_SET_COUNTER(&htim2, pwm_duty);

					Save_Mode_To_Flash(m); // 呼叫刚才写的函数保存进内部 Flash

					// 回传 OK\r\n
					HAL_UART_Transmit(&huart1, (uint8_t *)"OK\r\n", 4, 100);
				}
			}

			rx_flag = 0;
			rx_index = 0;
		}

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 71;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 999;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 65535;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = 0;
  sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = 0;
  if (HAL_TIM_Encoder_Init(&htim2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

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
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin : PB1 */
  GPIO_InitStruct.Pin = GPIO_PIN_1;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

// 串口接收中断回调函数
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        // 防止数组越界
        if (rx_index < 19) {
            rx_buffer[rx_index++] = rx_data; // 存入缓冲区

            if (rx_data == '\n') { // 判断是否收到换行符 (\n)
                rx_buffer[rx_index] = '\0'; // 添加字符串结束符
                rx_flag = 1;                // 标记一帧数据接收完成
            }
        } else {
            rx_index = 0; // 如果过长没收到换行，直接清空防溢出
        }

        // 重新开启下一次接收中断
        HAL_UART_Receive_IT(&huart1, &rx_data, 1);
    }
}

/* USER CODE END 4 */

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
