/****************************************************************************
 * OneNET云平台通信驱动 - onenet.c
 * 
 * 功能描述:
 *   实现与OneNET云平台的MQTT协议通信
 *   支持设备连接、属性上报、属性设置响应、消息订阅等功能
 *   智能停车场系统数据上报和命令接收的核心模块
 * 
 * 硬件配置:
 *   - 通过ESP8266 WiFi模块连接OneNET平台
 *   - 使用MQTT协议(1883端口)
 * 
 * 设备认证信息:
 *   - 产品ID: 53BV12EYcY
 *   - 设备名称: park1
 *   - 鉴权Token: 通过设备密钥生成
 * 
 * 作者: Bjmyhc
 * 日期: 2026-07-19
 ****************************************************************************/

#include "stm32f10x.h"
#include "esp8266.h"
#include "onenet.h"
#include "mqttkit.h"
#include "bsp_usart.h"
#include "bsp_delay.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"

/* LedEnable全局变量(定义在main.c中) */
extern uint8_t LEDEnable;

/* 设备配置(PROID, DEVID, TOKEN, 通信主题) */
#include "device_config.h"

/****************************************************************************
 * 函数名: OneNet_DevLink
 * 功能:   连接OneNET云平台
 * 参数:   无
 * 返回值: 0-连接成功, 1-连接失败
 * 说明:   使用MQTT协议连接OneNET平台
 *         连接参数: 产品ID, Token, 设备名称
 *         超时时间约250×5ms = 1.25秒
 ****************************************************************************/
_Bool OneNet_DevLink(void)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    unsigned char *dataPtr;
    _Bool status = 1;

    // 打印连接参数，便于调试
    Usart_Printf(USART_DEBUG, "OneNet_DevLink\r\n"
                 "PROID: %s, TOKEN: %s, DEVID: %s\r\n", PROID, TOKEN, DEVID);

    // 构建MQTT连接报文
    if (MQTT_PacketConnect(PROID, TOKEN, DEVID, 256, 1, MQTT_QOS_LEVEL0, NULL, NULL, 0, &mqttPacket) == 0)
    {
        // 发送连接报文到ESP8266
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 轮询等待平台CONNACK响应(超时1.25秒)
        {
            uint32_t connStartTick = Get_Tick();
            do {
                dataPtr = ESP8266_GetIPD(0);
                if (dataPtr != NULL) break;
                DelayXms(5);
            } while (Get_Tick() - connStartTick < 1250);
        }

        // 检查是否收到响应数据
        if (dataPtr != NULL)
        {
            // 判断响应类型是否为CONNACK
            if (MQTT_UnPacketRecv(dataPtr) == MQTT_PKT_CONNACK)
            {
                // 解析连接响应码，判断连接结果
                switch (MQTT_UnPacketConnectAck(dataPtr))
                {
                    case 0: Usart_Printf(USART_DEBUG, "Tips: 连接成功\r\n"); status = 0; break;
                    case 1: Usart_Printf(USART_DEBUG, "WARN: 连接失败，协议错误\r\n"); break;
                    case 2: Usart_Printf(USART_DEBUG, "WARN: 连接失败，非法clientid\r\n"); break;
                    case 3: Usart_Printf(USART_DEBUG, "WARN: 连接失败，服务器失败\r\n"); break;
                    case 4: Usart_Printf(USART_DEBUG, "WARN: 连接失败，用户名或密码错误\r\n"); break;
                    case 5: Usart_Printf(USART_DEBUG, "WARN: 连接失败，非法授权(check token)\r\n"); break;
                    default: Usart_Printf(USART_DEBUG, "ERR: 连接失败，未知错误\r\n"); break;
                }
            }
        }

        // 释放MQTT报文缓冲区
        MQTT_DeleteBuffer(&mqttPacket);
    }
    else
        Usart_Printf(USART_DEBUG, "WARN: MQTT_PacketConnect Failed\r\n");

    return status;
}

/****************************************************************************
 * 函数名: OneNet_Subscribe
 * 功能:   订阅OneNET平台主题
 * 参数:   topics - 主题数组指针
 *         topic_cnt - 主题数量
 * 返回值: 无
 * 说明:   订阅平台下发指令的主题
 *         使用QOS Level 0
 ****************************************************************************/
void OneNet_Subscribe(const char *topics[], unsigned char topic_cnt)
{
    unsigned char i = 0;

    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    // 打印所有要订阅的主题
    for (; i < topic_cnt; i++)
        Usart_Printf(USART_DEBUG, "Subscribe Topic: %s\r\n", topics[i]);

    // 构建MQTT订阅报文
    if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID, MQTT_QOS_LEVEL0, topics, topic_cnt, &mqttPacket) == 0)
    {
        // 发送订阅报文
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 释放缓冲区
        MQTT_DeleteBuffer(&mqttPacket);
    }
}

/****************************************************************************
 * 函数名: OneNet_Publish
 * 功能:   发布消息到OneNET平台
 * 参数:   topic - 发布主题
 *         msg - 消息内容(JSON格式)
 * 返回值: 无
 * 说明:   使用QOS Level 0发布消息
 *         消息保留标志: 1(保留)
 ****************************************************************************/
void OneNet_Publish(const char *topic, const char *msg)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    // 打印发布信息，便于调试
    Usart_Printf(USART_DEBUG, "Publish Topic: %s, Msg: %s\r\n", topic, msg);

    // 构建MQTT发布报文
    if (MQTT_PacketPublish(MQTT_PUBLISH_ID, topic, msg, strlen(msg), MQTT_QOS_LEVEL0, 0, 1, &mqttPacket) == 0)
    {
        // 发送发布报文
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 释放缓冲区
        MQTT_DeleteBuffer(&mqttPacket);
    }
}

/****************************************************************************
 * 函数名: OneNet_SendPropertyReply
 * 功能:   发送属性设置响应
 * 参数:   id - 请求ID
 *         code - 响应码(200=成功, 400=失败)
 *         msg - 响应消息
 * 返回值: 无
 * 说明:   平台下发属性设置后,设备必须回复此响应
 ****************************************************************************/
void OneNet_SendPropertyReply(const char *id, int code, const char *msg)
{
    char replyBuf[128];

    // 构建属性设置响应JSON格式
    sprintf(replyBuf, "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}", id, code, msg);

    // 打印响应信息
    Usart_Printf(USART_DEBUG, "Property Reply: %s\r\n", replyBuf);

    // 发布到属性设置响应主题
    OneNet_Publish(TOPIC_PROPERTY_SET_REPLY, replyBuf);
}

/****************************************************************************
 * 函数名: OneNet_RevPro
 * 功能:   处理平台下发的指令
 * 参数:   cmd - 接收到的原始MQTT数据包
 * 返回值: 无
 * 说明:   解析MQTT数据包,处理各种类型的消息
 *         包括: CMD指令、PUBLISH消息、PUBACK确认等
 ****************************************************************************/
void OneNet_RevPro(unsigned char *cmd)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    char *req_payload = NULL;
    char *cmdid_topic = NULL;

    unsigned short topic_len = 0;
    unsigned short req_len = 0;

    unsigned char type = 0;
    unsigned char qos = 0;
    static unsigned short pkt_id = 0;

    short result = 0;
    cJSON *json = NULL;
    cJSON *id_json = NULL;
    cJSON *params_json = NULL;

    char reqId[32] = "0";
    int respCode = 200;
    const char *respMsg = "success";

    // 解析MQTT报文类型
    type = MQTT_UnPacketRecv(cmd);
    switch (type)
    {
        // 处理CMD指令类型报文
        case MQTT_PKT_CMD:

            // 解析CMD指令
            result = MQTT_UnPacketCmd(cmd, &cmdid_topic, &req_payload, &req_len);
            if (result == 0)
            {
                // 打印指令信息
                Usart_Printf(USART_DEBUG, "cmdid: %s, req: %s, req_len: %d\r\n", cmdid_topic, req_payload, req_len);

                // 构建并发送CMD响应
                if (MQTT_PacketCmdResp(cmdid_topic, req_payload, &mqttPacket) == 0)
                {
                    Usart_Printf(USART_DEBUG, "Tips: 发送CmdResp\r\n");
                    ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                    MQTT_DeleteBuffer(&mqttPacket);
                }
            }
            break;

        // 处理PUBLISH消息类型报文（属性设置等）
        case MQTT_PKT_PUBLISH:

            // 解析PUBLISH报文
            result = MQTT_UnPacketPublish(cmd, &cmdid_topic, &topic_len, &req_payload, &req_len, &qos, &pkt_id);
            if (result == 0)
            {
                // 打印主题和负载信息
                Usart_Printf(USART_DEBUG, "topic: %s, payload: %s\r\n", cmdid_topic, req_payload);

                // 解析JSON格式的负载数据
                json = cJSON_Parse(req_payload);
                if (json == NULL)
                {
                    Usart_Printf(USART_DEBUG, "JSON parse failed\r\n");
                    respCode = 400;
                    respMsg = "invalid json";
                }
                else
                {
                    // 提取请求ID
                    id_json = cJSON_GetObjectItem(json, "id");
                    if (id_json != NULL && id_json->valuestring != NULL)
                    {
                        strncpy(reqId, id_json->valuestring, sizeof(reqId) - 1);
                    }

                    // 提取params参数
                    params_json = cJSON_GetObjectItem(json, "params");
                    if (params_json == NULL)
                    {
                        Usart_Printf(USART_DEBUG, "No params found\r\n");
                        respCode = 400;
                        respMsg = "no params";
                    }
                    else
                    {
                        /* 处理LedEnable属性设置 */
                        cJSON *ledEnableJson = cJSON_GetObjectItem(params_json, "LedEnable");
                        if (ledEnableJson != NULL)
                        {
                            LEDEnable = ledEnableJson->valueint;
                            Usart_Printf(USART_DEBUG, "LEDEnable set to: %d\r\n", LEDEnable);
                        }
                    }
                }

                // 发送属性设置响应
                OneNet_SendPropertyReply(reqId, respCode, respMsg);
            }
            break;

        // 处理QOS1消息确认
        case MQTT_PKT_PUBACK:

            if (MQTT_UnPacketPublishAck(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT Publish Send OK\r\n");
            break;

        // 处理QOS2消息阶段1：收到PUBREC
        case MQTT_PKT_PUBREC:

            if (MQTT_UnPacketPublishRec(cmd) == 0)
            {
                Usart_Printf(USART_DEBUG, "Tips: Rev PublishRec\r\n");
                // 发送PUBREL响应
                if (MQTT_PacketPublishRel(MQTT_PUBLISH_ID, &mqttPacket) == 0)
                {
                    Usart_Printf(USART_DEBUG, "Tips: Send PublishRel\r\n");
                    ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                    MQTT_DeleteBuffer(&mqttPacket);
                }
            }
            break;

        // 处理QOS2消息阶段2：收到PUBREL
        case MQTT_PKT_PUBREL:

            if (MQTT_UnPacketPublishRel(cmd, pkt_id) == 0)
            {
                Usart_Printf(USART_DEBUG, "Tips: Rev PublishRel\r\n");
                // 发送PUBCOMP完成确认
                if (MQTT_PacketPublishComp(MQTT_PUBLISH_ID, &mqttPacket) == 0)
                {
                    Usart_Printf(USART_DEBUG, "Tips: Send PublishComp\r\n");
                    ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                    MQTT_DeleteBuffer(&mqttPacket);
                }
            }
            break;

        // 处理QOS2消息阶段3：收到PUBCOMP
        case MQTT_PKT_PUBCOMP:

            if (MQTT_UnPacketPublishComp(cmd) == 0)
            {
                Usart_Printf(USART_DEBUG, "Tips: Rev PublishComp\r\n");
            }
            break;

        // 处理订阅确认
        case MQTT_PKT_SUBACK:

            if (MQTT_UnPacketSubscribe(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT Subscribe OK\r\n");
            else
                Usart_Printf(USART_DEBUG, "Tips: MQTT Subscribe Err\r\n");
            break;

        // 处理取消订阅确认
        case MQTT_PKT_UNSUBACK:

            if (MQTT_UnPacketUnSubscribe(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT UnSubscribe OK\r\n");
            else
                Usart_Printf(USART_DEBUG, "Tips: MQTT UnSubscribe Err\r\n");
            break;

        // 未知报文类型
        default:
            result = -1;
            break;
    }

    // 清空ESP8266接收缓冲区
    ESP8266_Clear();

    // 如果报文类型未知，直接返回
    if (result == -1)
        return;

    // 释放CMD和PUBLISH报文的动态内存
    if (type == MQTT_PKT_CMD || type == MQTT_PKT_PUBLISH)
    {
        MQTT_FreeBuffer(cmdid_topic);
        MQTT_FreeBuffer(req_payload);
    }
}
