/*
 * usart3task.c
 *
 *  Created on: 2018年5月3日
 *      Author: Administrator
 */

#include "usart3task.h"
#include "usart1task.h"//调试printf
#include "ESP8266.h"
#include "task_config.h"

#include "embARC.h"
#include "embARC_debug.h"
#include "dev_uart.h"

#include "malloc.h"
#include "string.h"

#include "stdlib.h"

#include "strpos.h"

uint8_t usart3_data_all[USART3_NUM][USART3_PER_LEN];//串口1缓冲buff
uint8_t usart3_buff_flag=1;//标记哪些数组被使用了，下次往哪个数组中存 默认使用第一个数组
usart_dat usart3_data;
uint8_t usart3_time_out=0;//串口1接收超时计时

uint16_t USART3_RX_STA=0;       //接收状态标记

SemaphoreHandle_t SendenableMutexSemaphore;	//互斥信号量

TimerHandle_t OneShot_CanServer_Handle;

#if EN_USART3_RX   //如果使能了接收

TaskHandle_t FREE_ERRORMEM_Handler;
TaskHandle_t USART3_Handler;
TaskHandle_t USART3_send_Handler;

DEV_UART *uart3;
static uint8_t esp8266_rx_buffer;
static DEV_BUFFER esp8266_uart_int_buffer;
uint16_t testbuffer[5]={0};

void occupy_buff3(void);
uint32_t uart3_init(uint32_t bound)
{
	occupy_buff3();
	uart3 = uart_get_dev(DW_UART_0_ID);

		if (uart3 == NULL) {
			EMBARC_PRINTF("Failed to get device of uart0 for uart3.\r\n");
			return -1;
		}

		if (uart3->uart_open(bound) == E_OPNED) {
			uart3->uart_control(UART_CMD_SET_BAUD, (void *)(bound));
		}

		DEV_BUFFER_INIT(&esp8266_uart_int_buffer, &esp8266_rx_buffer, 1);
		if (uart3->uart_control(UART_CMD_SET_RXINT_BUF, (void *) & esp8266_uart_int_buffer) != E_OK) {
			return -1;
		}
		int_handler_install(INTNO_UART0, USART3_IRQHandler);
		int_pri_set(INTNO_UART0, (-2));
		if (uart3->uart_control(UART_CMD_SET_RXINT, (void *)(1)) != E_OK) {
			return -1;
		}
		return 0;
}

//找到未被使用的缓冲buff，
//返回0-（USART3_NUM-1）表示相应的数组可以使用
//返回USART3_NUM表示没有空闲缓冲buff
//存到 buff_now中
void occupy_buff3(void)
{
	uint8_t i,ret=USART3_NUM;
	for(i=0;i<USART3_NUM;i++)
	{
		if(!(usart3_buff_flag&(0x01<<i)))
		{
			ret  = i;
			usart3_buff_flag|=(0x01<<i);
			break;
		}
	}
	usart3_data.buff_now = ret;
}

//处理完缓冲buff数据后，释放buff的占用状态
void release_buff3(uint8_t buff_num)
{
	if(buff_num<USART3_NUM)
	{
		memset(usart3_data_all[buff_num],0,USART3_PER_LEN);//清除数据接收缓冲区USART_RX_BUF,用于下一次数据接收
		usart3_buff_flag&=~(0x01<<buff_num);

	}
	if(usart3_data.buff_now==USART3_NUM) occupy_buff3();
}

//extern QueueHandle_t Message_Queue;	//信息队列句柄
QueueHandle_t USART3_Queue;

void USART3_IRQHandler(void)                	//串口1中断服务程序
{
	uint8_t Res3=0;
	BaseType_t xHigherPriorityTaskWoken;
	uart3->uart_read(&Res3, 1);	//读取接收到的数据
	usart3_time_out=0x80;//计数器清空          				//计数器清空
#if 1
		if(usart3_data.buff_now<USART3_NUM)
		{
			usart3_data_all[usart3_data.buff_now][USART3_RX_STA++] = Res3;
		}
		else
		{
//			occupy_buff();
//			printf("USART3buff no space\r\n");
		}

		if(USART3_RX_STA>USART3_PER_LEN-1)
		{
			usart3_time_out=0x00;//接收完成一次数据，不再进行超时计时。下次收到数据再计时
			usart3_data.len=USART3_RX_STA&0x7fff;
			xQueueSendFromISR(USART3_Queue,&usart3_data,&xHigherPriorityTaskWoken);//向队列中发送数据
			USART3_RX_STA = 0;
			occupy_buff3();
			portYIELD_FROM_ISR();//如果需要的话进行一次任务切换
		}

#endif
}
#endif

//QueueHandle_t USART3_Task_Queue;//串口task处理后的数据队列 都是分包好的一帧一帧的数据
QueueHandle_t USART3_Task_server_Queue;//存储服务器数据
QueueHandle_t USART3_Task_rn_other_Queue;//存储\r\n数据和没有格式的数据
USART_TASK_PROCESS USART3_task_process=USART_DATA_IDEL;//数据处理流程标记
uint8_t usart3_task_time_out=0;//串口1 task接收超时计时
uint16_t server_data_remain3;//server data还有多少没有复制过来
usart_dat USART3_temp;//接收底层串口缓冲数据队列
usart_task_data USART3_task_temp;//发送到USART3task队列中

void USART3_task(void *pvParameters)
{

		uint8_t temp_pos3;//暂存标志位的位置信息
		uint8_t data_num_remain3=0;//当前处理了数据.剩余数据长度 要把接收到的USART3_temp.len长度数据处理完
		uint8_t* data_p3;//存储数据的地址 申请内存时，根据包内信息申请对应的大小内存
		uint8_t* data_temp3;//底层串口数据指针
		BaseType_t err;
		dprintf("USART3_task success\r\n");
    while(1)
    {
       if(xQueueReceive(USART3_Queue,&USART3_temp,portMAX_DELAY))//接收到底层串口数据
			 {
				 data_num_remain3 = USART3_temp.len;
				 data_temp3 = usart3_data_all[USART3_temp.buff_now];

					switch(USART3_task_process)
					{
						case USART_DATA_IDEL:
						{
							CHECK_BUFF:while(data_num_remain3)
							{
								temp_pos3 = findpos(server_start,4,data_temp3,data_num_remain3);
								if(temp_pos3==0xff)//没有服务器数据的起始标志 认为是普通数据
								{
									temp_pos3=findpos(rn_stop,2,data_temp3,data_num_remain3);
									if(temp_pos3==0xFF)//没有\r\n结束标记
									{
										if(data_num_remain3<50)//数据量不多的时候进行直接发送
										{

											{
												data_p3=pvPortMalloc(data_num_remain3);//不传递\r\n出去，只传递前面的数据
												memcpy(data_p3,data_temp3,data_num_remain3);
												USART3_task_temp.len = data_num_remain3;
												USART3_task_temp.pstr=data_p3;
												USART3_task_temp.type=USART_OTHER;
												err=xQueueSend(USART3_Task_rn_other_Queue,&USART3_task_temp,10);
												if(err==errQUEUE_FULL)   	//发送消息帧
												{
													//发送失败需要free内存
													vPortFree(USART3_task_temp.pstr);
														dprintf("队列USART3_Task_rn_other_Queue已满，数据发送OTHER失败!\r\n");
												}
											}
										}

										data_num_remain3 = 0;

										break;
									}
									else//寻找到\r\n 发送当前帧，判断缓冲buff剩余数据是否处理完毕，没有处理完毕就继续 USART_DATA_IDEL 的处理
									{
											if(temp_pos3>0)
											{
												data_p3=pvPortMalloc(temp_pos3);//不传递\r\n出去，只传递前面的数据
												memcpy(data_p3,data_temp3,temp_pos3);
												USART3_task_temp.len = temp_pos3;
												USART3_task_temp.pstr=data_p3;
												USART3_task_temp.type=USART_RN;
												err=xQueueSend(USART3_Task_rn_other_Queue,&USART3_task_temp,10);
												if(err==errQUEUE_FULL)   	//发送消息帧
												{
														//发送失败需要free内存
														vPortFree(USART3_task_temp.pstr);
														dprintf("队列USART3_Task_rn_other_Queue已满，数据发送RN失败!\r\n");
												}
											}
											data_num_remain3-=temp_pos3+2;
											data_temp3+=temp_pos3+2;

									}
								}
								else if(temp_pos3==0x00)//起始位置就是起始标志
								{
									server_data_remain3 = ((data_temp3[4])<<8)+data_temp3[5];//这一帧数据长度
									if(data_num_remain3>5)//至少包含长度信息，不然就不能继续
									{
									if(server_data_remain3>data_num_remain3)//一帧跨越几个包，需要几个包进行组帧
									{
										//申请内存，复制信息到内存中，等待下一个包
										//server帧：包含起始标志和结束标志
										usart3_task_time_out=0x80;
										data_p3=pvPortMalloc(server_data_remain3);
										USART3_task_temp.len = server_data_remain3;
										USART3_task_temp.pstr=data_p3;
										memcpy(data_p3,data_temp3,data_num_remain3);
										data_p3+=data_num_remain3;
										server_data_remain3-=data_num_remain3;//帧还需要的长度
										USART3_task_process = USART_DATA_SERVER_START;//SERVER组帧的进程
										data_num_remain3 = 0;
										break;
									}
									else//一帧server 在一个包里面就能组完
									{
										//先判断结束位是否正确，再执行操作
										if(strsame(server_stop,data_temp3+server_data_remain3-4,4))//验证失败，是否把数据当做other数据发出？这里的处理是丢弃
										{

											data_p3=pvPortMalloc(server_data_remain3);
											USART3_task_temp.len = server_data_remain3;
											USART3_task_temp.pstr=data_p3;
											USART3_task_temp.type=USART_SERVER;
											memcpy(data_p3,data_temp3,server_data_remain3);
											err=xQueueSend(USART3_Task_server_Queue,&USART3_task_temp,10);
											if(err==errQUEUE_FULL)   	//发送消息帧
											{
												  //发送失败需要free内存
												  vPortFree(USART3_task_temp.pstr);
													dprintf("队列USART3_Task_server_Queue已满，数据发送单包内一帧server数据失败!\r\n");
											}
										}
									}
										{
											data_num_remain3-=server_data_remain3;
											data_temp3+=server_data_remain3;
										}
									}
									else//不包括起始的6位 直接丢弃
									{
										data_num_remain3 = 0;
										break;
									}

								}
								else//起始位置在中间 也就是服务器数据之前有其他数据 调整到起始位置就是起始标志
								{
										data_num_remain3-=temp_pos3;
										data_temp3+=temp_pos3;
								}
						}
						//处理完数据，释放buff的占用
						release_buff3(USART3_temp.buff_now);//释放buff的占用状态
					}
							break;
						case USART_DATA_SERVER_START:
						{
							{
								if(server_data_remain3>data_num_remain3)//跨包组帧
								{
										usart3_task_time_out=0x80;
										memcpy(data_p3,data_temp3,data_num_remain3);
										data_p3+=data_num_remain3;
										server_data_remain3-=data_num_remain3;//帧还需要的长度
										USART3_task_process = USART_DATA_SERVER_START;//SERVER组帧的进程
										data_num_remain3 = 0;
										break;
								}
								else//一包内结束组帧
								{
									usart3_task_time_out=0x00;
									USART3_task_process = USART_DATA_IDEL;
									memcpy(data_p3,data_temp3,server_data_remain3);
									if(strsame(server_stop,USART3_task_temp.pstr+USART3_task_temp.len-4,4))
									{
											USART3_task_temp.type=USART_SERVER;
											err=xQueueSend(USART3_Task_server_Queue,&USART3_task_temp,10);
											if(err==errQUEUE_FULL)   	//发送消息帧
											{
													//发送失败需要free内存
													vPortFree(USART3_task_temp.pstr);
													dprintf("队列USART3_Task_server_Queue已满，数据发送多包内一帧server数据失败!\r\n");
											}
									}
									else//数据不对，需要free内存
									{
										vPortFree(USART3_task_temp.pstr);
									}
									data_num_remain3-=server_data_remain3;
									data_temp3+=server_data_remain3;

									{//组帧完毕还有数据没有处理
										if(data_num_remain3)
										{//再次启动数据检查
											goto CHECK_BUFF;//不一定有效，继续检查buff数据
//											data_num_remain =0;//丢弃后面的数据
										}
									}
								}
							}
						//处理完数据，释放buff的占用
						release_buff3(USART3_temp.buff_now);//释放buff的占用状态
						}
							break;
					}
			 }

			vTaskDelay(50);      //延时10ms，也就是10个时钟节拍USART3_task_delay
    }
}

//二值信号量句柄
SemaphoreHandle_t BinarySemaphore_free;	//二值信号量句柄


//USART3_task中会存在多包组帧数据出现错误的情况，需要进行free 错误内存的操作
void FREE_ERRORMEM_task(void *pvParameters)
{
	BaseType_t err;
	dprintf("FREE_ERRORMEM_task success\r\n");
	while(1)
	{
		if(BinarySemaphore_free!=NULL)
		{
			err=xSemaphoreTake(BinarySemaphore_free,portMAX_DELAY);	//获取信号量
//			err=xSemaphoreTake(BinarySemaphore_free,0);	//获取信号量
			if(err==pdTRUE)										//获取信号量成功
			{
				USART3_task_process = USART_DATA_IDEL;
				vPortFree(USART3_task_temp.pstr);//在中断中free会出错，需要到外部task中进行操作
				dprintf("free_mem sucess!\r\n");
			}
			else if(err==pdFALSE)
			{
				vTaskDelay(FREE_ERRORMEM_delay);     //延时10ms，也就是10个时钟节拍
			}
		}
		      //延时10ms，也就是10个时钟节拍
	}
}

//发送队列，把数据放到发送队列中，进行发送


QueueHandle_t USART3_Send_Queue;//串口3发送队列
//串口3发送消息任务
void USART3_send_task(void *pvParameters)
{
	usart_task_data usart3_send_temp;
	char at_cipsend[30];

	dprintf("usart3_send start\r\n");
	    while(1)
    {
			if(xQueueReceive(USART3_Send_Queue,&usart3_send_temp, 0))
//       if(xQueueReceive(USART3_Send_Queue,&usart3_send_temp, portMAX_DELAY))//发送队列有数据需要发送
			 {
				 if(usart3_send_temp.type ==USART_SERVER)
				 {
					if(touch_flag&(0x0001<<14))
					 {
						if(xSemaphoreTake(SendenableMutexSemaphore,portMAX_DELAY))//一旦有一个没有收到回执，就会再也不发送  开启一个定时器，超时自动释放，让数据能发送
						{
							 xTimerStart(OneShot_CanServer_Handle,10);
						}
					 }

					 {
							dprintf("send server\r\n");
					    sprintf(at_cipsend,"AT+CIPSEND=%d\r\n",usart3_send_temp.len);
					    u3_senddata((uint8_t*)at_cipsend,strlen(at_cipsend));
					    vTaskDelay(50); //避免busy问题 150
					    free_usart3_rn_other_queue();//重入问题
							u3_senddata(usart3_send_temp.pstr,usart3_send_temp.len);
					 }
				 }
				else if(usart3_send_temp.type ==USART_RN)
				{
					 u3_senddata(usart3_send_temp.pstr,usart3_send_temp.len);
					if(usart3_send_temp.type ==USART_RN) u3_senddata((uint8_t*)"\r\n",2);

				}
				vPortFree(usart3_send_temp.pstr);

			 }
			 vTaskDelay(USART3_SEND_delay );
		}

}

void CanServer_Callback(TimerHandle_t xTimer)
{
	xSemaphoreGive(SendenableMutexSemaphore);					//释放信号量
}

#define U3toU1 0

 //串口3，senddata
void u3_senddata(uint8_t *data,uint16_t length)
{
	uint16_t j;
	uart3 ->uart_write(data,length);
}

//end USART3task.c

