#include "yahboom_camera.h"
#include "app_mywifi.h"
#include "app_myhttpd.hpp"
#include "app_mymdns.h"

#include "esp_log.h"
#include "driver/spi_common.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_flash.h"

#include "my_usart.h"
#include "my_usart1_user.h"
#include "my_user_iic.h"
#include "mykey.h"
#include <cstring>


static QueueHandle_t xQueueAIFrame = NULL;

static const char TAG[] = "main_AI_version";
char Version[] = "AI_V1.5.1";
uint16_t wifi_Mode = 2; 
extern uint8_t sta_ip_connect[4];

extern "C" void app_main(void)
{
    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));

    app_mywifi_main();

    //摄像头相关
    my_register_camera(PIXFORMAT_RGB565, FRAMESIZE_QVGA, 2, xQueueAIFrame);// FRAMESIZE_VGA,会卡， _240X240
    app_mymdns_main();

    register_httpd(xQueueAIFrame, NULL, true);//图传服务

    //直接把版本号发送出来+ 串口助手也能查询
    printf("YAHBOOM Board Ver:%s\r\n",Version);

    printf("sta_ip:%d.%d.%d.%d\r\n",sta_ip_connect[0],sta_ip_connect[1],sta_ip_connect[2],sta_ip_connect[3]);

    printf("ap_ip:192.168.4.1\r\n"); //固定就好
}
