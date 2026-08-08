/****************************************************************************
 * OneNET平台MQTT协议驱动 - onenet.c
 *
 * 功能描述:
 *   实现基于OneNET平台的MQTT协议通信
 *   支持设备接入、属性上报、下行命令处理和响应消息
 *   用于智能停车场系统的车位状态上报与控制
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

/* LEDEnable全局变量(定义在app_tasks.c) */
extern uint8_t LEDEnable;

/* 属性上报/下发主题引用(定义在 app_tasks.c):
 * 属性上报、属性设置、回复等主题由应用层定义, 本文件仅引用使用 */
extern volatile uint8_t StatusChanged;

/* 设备配置(PROID, DEVID, TOKEN, WiFi等) */
#include "device_config.h"

/* MQTT CONNACK等待超时(ms)
 * 原1250ms对LoRa链路偏短, CONNACK常超时被误判
 * Lora链路慢, 需要更长等待时间 */
#define MQTT_CONNACK_TIMEOUT   8000

/****************************************************************************
 * 函数名: OneNet_DevLink
 * 功能:   连接OneNET平台
 * 参数:   无
 * 返回值: 0-连接成功, 1-连接失败
 * 说明:   使用MQTT协议连接OneNET平台
 *         连接参数: 产品ID, Token, 设备名称
 *         等待时间约250次×5ms = 1.25s
 ****************************************************************************/
_Bool OneNet_DevLink(void)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    unsigned char *dataPtr;
    _Bool status = 1;

    // 打印设备连接信息
    Usart_Printf(USART_DEBUG, "OneNet_DevLink\r\n"
                 "PROID: %s, TOKEN: %s, DEVID: %s\r\n", PROID, TOKEN, DEVID);

    // 构建MQTT连接报文
    // keepalive=60s: 原256s过长, LoRa链路空闲时TCP易被中间设备断开,
    // 需定时发送PINGREQ维持连接(见Wifi_Task的60s心跳)
    if (MQTT_PacketConnect(PROID, TOKEN, DEVID, 60, 1, MQTT_QOS_LEVEL0, NULL, NULL, 0, &mqttPacket) == 0)
    {
        // 将连接报文发送给ESP8266
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 轮询等待平台CONNACK响应(超时1.25s)
        {
            uint32_t connStartTick = Get_Tick();
            do {
                dataPtr = ESP8266_GetIPD(0);
                if (dataPtr != NULL) break;
                DelayXms(5);
            } while (Get_Tick() - connStartTick < MQTT_CONNACK_TIMEOUT);
        }

        // 成功收到响应
        if (dataPtr != NULL)
        {
            // 检查响应类型是否为CONNACK
            if (MQTT_UnPacketRecv(dataPtr) == MQTT_PKT_CONNACK)
            {
                // 解析连接结果并打印
                switch (MQTT_UnPacketConnectAck(dataPtr))
                {
                    case 0: Usart_Printf(USART_DEBUG, "Tips: 连接成功\r\n"); status = 0; break;
                    case 1: Usart_Printf(USART_DEBUG, "WARN: 连接失败,协议错误\r\n"); break;
                    case 2: Usart_Printf(USART_DEBUG, "WARN: 连接失败,非法clientid\r\n"); break;
                    case 3: Usart_Printf(USART_DEBUG, "WARN: 连接失败,服务器不可用\r\n"); break;
                    case 4: Usart_Printf(USART_DEBUG, "WARN: 连接失败,用户名或密码错误\r\n"); break;
                    case 5: Usart_Printf(USART_DEBUG, "WARN: 连接失败,未授权(check token)\r\n"); break;
                    default: Usart_Printf(USART_DEBUG, "ERR: 连接失败,未知错误\r\n"); break;
                }
                /* 消费CONNACK报文: CONNACK固定4字节, GetIPD用peek取数据,
                 * 解析后需手动弹出, 否则残留字节影响后续报文解析 */
                ESP8266_Consume(4);
            }
        }

        // 释放MQTT报文内存
        MQTT_DeleteBuffer(&mqttPacket);
    }
    else
        Usart_Printf(USART_DEBUG, "WARN: MQTT_PacketConnect Failed\r\n");

    return status;
}

/****************************************************************************
 * 函数名: OneNet_Subscribe
 * 功能:   订阅OneNET平台主题
 * 参数:   topics - 订阅的主题指针
 *         topic_cnt - 主题数量
 * 返回值: 无
 * 说明:   订阅平台下发的指令
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
    // QoS1 subscription: 使用QoS1订阅保证指令可达, 平台重发时需应答PUBACK,
    // 避免LoRa链路丢失订阅确认(QoS0无确认, 丢失不可知)
    if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID, MQTT_QOS_LEVEL1, topics, topic_cnt, &mqttPacket) == 0)
    {
        // 发送订阅报文
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 释放订阅报文内存
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
 *         消息最大长度: 1(报文头)
 ****************************************************************************/
void OneNet_Publish(const char *topic, const char *msg)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};

    // 打印要发布的消息信息
    Usart_Printf(USART_DEBUG, "Publish Topic: %s, Msg: %s\r\n", topic, msg);

    // 构建MQTT发布报文
    if (MQTT_PacketPublish(MQTT_PUBLISH_ID, topic, msg, strlen(msg), MQTT_QOS_LEVEL0, 0, 1, &mqttPacket) == 0)
    {
        // 发送发布报文
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        // 释放发布报文内存
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
 * 说明:   平台下发属性设置后,设备需要回复响应
 ****************************************************************************/
void OneNet_SendPropertyReply(const char *id, int code, const char *msg)
{
    char replyBuf[128];

    // 构建属性设置响应JSON格式
    sprintf(replyBuf, "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}", id, code, msg);

    // 打印响应信息
    Usart_Printf(USART_DEBUG, "Property Reply: %s\r\n", replyBuf);

    // 发布属性设置响应到平台
    OneNet_Publish(TOPIC_PROPERTY_SET_REPLY, replyBuf);
}

/****************************************************************************
 * 函数名: OneNet_RevPro
 * 功能:   处理平台下发的指令
 * 参数:   cmd - 接收到的原始MQTT报文
 * 返回值: 无
 * 说明:   解析MQTT报文,处理上行和下行指令
 *         类型: CMD指令, PUBLISH信息和PUBACK确认
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

    uint16_t consumedLen = 0;
    uint8_t isKnownType = 1;   /* 标记当前报文类型是否在switch中处理(未知类型只消费1字节) */

    /* QoS1去重: 记录已确认PUBACK的报文id, 用于识别重传.
     * 重传PUBLISH不再执行, 只回复PUBACK, 防止重复执行, 避免LoRa链路下重复命令造成重复操作 */
    static uint16_t lastPktId = 0;
    static uint8_t lastPktIdValid = 0;

    // 解析MQTT报文类型
    type = MQTT_UnPacketRecv(cmd);

    /* 解析报文剩余长度(类型+长度+载荷), 用于精确消费缓冲区,
     * 避免整批Clear()丢失数据. 配合"垃圾+真命令"同批到达时,
     * 只弹出已处理报文, 保留剩余命令继续解析 */
    unsigned char lenBytes = 0;
    {
        uint32_t remLen = 0;
        uint32_t multiplier = 1;
        unsigned char *p = cmd + 1;
        while (1)
        {
            if (lenBytes >= 4) { lenBytes = 0xFF; break; }   /* 异常: 长度编码超过4字节 */
            remLen += (uint32_t)(*p & 0x7F) * multiplier;
            multiplier <<= 7;
            lenBytes++;
            if (!(*p & 0x80)) break;
            p++;
        }
        if (lenBytes == 0xFF || remLen > ESP8266_BUF_SIZE)
            consumedLen = 0;    /* 异常报文: 只消费1个字节, 防误吞 */
        else
            consumedLen = (uint16_t)(1 + lenBytes + remLen);
    }

    switch (type)
    {
        // 收到CMD指令
        case MQTT_PKT_CMD:

            // 解析CMD指令
            result = MQTT_UnPacketCmd(cmd, &cmdid_topic, &req_payload, &req_len);
            if (result == 0)
            {
                // 打印指令信息
                Usart_Printf(USART_DEBUG, "cmdid: %s, req: %s, req_len: %d\r\n", cmdid_topic, req_payload, req_len);

                // 构建CMD响应
                if (MQTT_PacketCmdResp(cmdid_topic, req_payload, &mqttPacket) == 0)
                {
                    Usart_Printf(USART_DEBUG, "Tips: 收到CmdResp\r\n");
                    ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                    MQTT_DeleteBuffer(&mqttPacket);
                }
            }
            break;

        // 收到PUBLISH信息(属性设置)
        case MQTT_PKT_PUBLISH:

            // 解析PUBLISH报文
            result = MQTT_UnPacketPublish(cmd, &cmdid_topic, &topic_len, &req_payload, &req_len, &qos, &pkt_id);
            if (result == 0)
            {
                // 清理payload尾部\0后杂讯: MQTT_UnPacketPublish返回的req_payload可能带尾部杂讯,
                // 这些杂讯会导致JSON解析失败, 需要先截断到有效内容
                /* Key fix: clean payload tail of invisible garbage (LoRa fragment issue) */
                {
                    int16_t i;
                    int16_t validEnd = -1;
                    for (i = req_len - 1; i >= 0; i--)
                    {
                        unsigned char ch = req_payload[i];
                        if (ch >= 32 && ch != 127)
                        {
                            validEnd = i;
                            break;
                        }
                    }
                    if (validEnd >= 0)
                        req_payload[validEnd + 1] = '\0';
                    else
                        req_payload[0] = '\0';
                    req_len = (validEnd >= 0) ? (uint16_t)(validEnd + 1) : 0;
                }
                /* 关键修复: LoRa链路切碎/杂讯污染时, 收到空topic或payload
                 * 的伪PUBLISH报文(常见 0x30 0x02 0x00 0x00). 如果回复400,
                 * 网页会误判为"命令失败"! 正确处理:
                 *   - 空报文(空id"0")不再回复400, 静默丢弃
                 *   - 只回复正常命令的PUBACK, 让QoS1平台重发完整命令
                 * 不回复PUBACK, 平台超时会重发完整命令, 不再误导网页 */
                if (cmdid_topic == NULL || cmdid_topic[0] == '\0' ||
                    req_payload == NULL || req_len == 0)
                {
                    /* 打印前12字节hex, 方便定位伪PUBLISH报文来源
                     * (LoRa切碎/杂讯污染/平台重发), 便于排查 */
                    unsigned char dbgIdx;
                    Usart_Printf(USART_DEBUG, "Discard invalid publish (empty topic/payload), raw: ");
                    for (dbgIdx = 0; dbgIdx < 12; dbgIdx++)
                        Usart_Printf(USART_DEBUG, "%02X ", cmd[dbgIdx]);
                    Usart_Printf(USART_DEBUG, "\r\n");
                    /* MQTT_UnPacketPublish成功但校验不通过(remain_len
                     * 异常), 需整包消费. 若不消费, 残留数据
                     * 会堆积, payload可能被后续Drop partial+Clear,
                     * 阻塞后续真实命令的接收 */
                    break;
                }

                Usart_Printf(USART_DEBUG, "topic: %s, payload[%d]: %s\r\n", cmdid_topic, req_len, req_payload);

                // 解析JSON并执行业务逻辑
                json = cJSON_Parse((const char *)req_payload);
                if (json == NULL)
                {
                    /* payload损坏(LoRa切碎或含0x00)时JSON解析失败:
                     * 静默丢弃, 不回复PUBACK也不回400! 回400会让网页误判
                     * 为"命令失败", 不回PUBACK平台会重发完整命令 */
                    Usart_Printf(USART_DEBUG, "JSON parse failed (corrupt payload), drop, no ack\r\n");
                    /* 解析成功(是干净命令), 继续执行业务逻辑 */
                    break;
                }
                else
                {
                    /* QoS1去重: 重复报文(已确认PUBACK)的pkt_id, 跳过执行.
                     * 只回复PUBACK, 避免LoRa链路下重复执行命令 */
                    if (qos == MQTT_QOS_LEVEL1 && lastPktIdValid && lastPktId == pkt_id)
                    {
                        if (MQTT_PacketPublishAck(pkt_id, &mqttPacket) == 0)
                        {
                            Usart_Printf(USART_DEBUG, "Dup QoS1 publish, ack pkt_id=%u\r\n", pkt_id);
                            ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                            MQTT_DeleteBuffer(&mqttPacket);
                        }
                        break;   /* 已去重: 跳过执行, 不消费payload */
                    }
                    // 提取命令ID
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
                        /* 执行LedEnable设置 */
                        cJSON *ledEnableJson = cJSON_GetObjectItem(params_json, "LedEnable");
                        if (ledEnableJson != NULL)
                        {
                            LEDEnable = ledEnableJson->valueint;
                            Usart_Printf(USART_DEBUG, "LEDEnable set to: %d\r\n", LEDEnable);
                            /* 状态已改变: 立即上报新状态, 触发
                             * 网页端数据同步更新(即使未到15s上报周期) */
                            StatusChanged = 1;
                        }
                    }
                }

                /* QoS1确认: 仅在"报文完整且JSON解析成功"时才回复PUBACK.
                 * 污染报文/解析失败不回复, 平台会重发QoS1报文,
                 * 保证命令最终被正确执行 */
                if (qos == MQTT_QOS_LEVEL1)
                {
                    lastPktId = pkt_id;
                    lastPktIdValid = 1;
                    if (MQTT_PacketPublishAck(pkt_id, &mqttPacket) == 0)
                    {
                        Usart_Printf(USART_DEBUG, "QoS1 publish ack, pkt_id=%u\r\n", pkt_id);
                        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                        MQTT_DeleteBuffer(&mqttPacket);
                    }
                }
                // 发送属性设置响应
                OneNet_SendPropertyReply(reqId, respCode, respMsg);
            }
            break;

        // 收到QOS1消息确认
        case MQTT_PKT_PUBACK:

            if (MQTT_UnPacketPublishAck(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT Publish Send OK\r\n");
            break;

        // 收到QOS2消息阶段1(已收PUBREC)
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

        // 收到QOS2消息阶段2(已收PUBREL)
        case MQTT_PKT_PUBREL:

            if (MQTT_UnPacketPublishRel(cmd, pkt_id) == 0)
            {
                Usart_Printf(USART_DEBUG, "Tips: Rev PublishRel\r\n");
                // 发送PUBCOMP响应
                if (MQTT_PacketPublishComp(MQTT_PUBLISH_ID, &mqttPacket) == 0)
                {
                    Usart_Printf(USART_DEBUG, "Tips: Send PublishComp\r\n");
                    ESP8266_SendData(mqttPacket._data, mqttPacket._len);
                    MQTT_DeleteBuffer(&mqttPacket);
                }
            }
            break;

        // 收到QOS2消息阶段3(已收PUBCOMP)
        case MQTT_PKT_PUBCOMP:

            if (MQTT_UnPacketPublishComp(cmd) == 0)
            {
                Usart_Printf(USART_DEBUG, "Tips: Rev PublishComp\r\n");
            }
            break;

        // 收到订阅确认
        case MQTT_PKT_SUBACK:

            if (MQTT_UnPacketSubscribe(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT Subscribe OK\r\n");
            else
                Usart_Printf(USART_DEBUG, "Tips: MQTT Subscribe Err\r\n");
            break;

        // 收到退订确认
        case MQTT_PKT_UNSUBACK:

            if (MQTT_UnPacketUnSubscribe(cmd) == 0)
                Usart_Printf(USART_DEBUG, "Tips: MQTT UnSubscribe OK\r\n");
            else
                Usart_Printf(USART_DEBUG, "Tips: MQTT UnSubscribe Err\r\n");
            break;

        // PINGRESP(0xD0 0x00): remainLen=0, consumedLen=1+1+0=2
        case MQTT_PKT_PINGRESP:
            break;

        default:
            result = -1;
            isKnownType = 0;
            break;
    }

    /* 消费处理后的数据: 精确弹出已处理字节, 保留剩余数据.
     * - 未知类型/异常报文: 只消费1字节, 防误吞
     * - MQTT报文解析失败(UnPacketPublish返回非255): remain_len被污染,
     *   只消费固定头, 不按错误长度整吞
     * - MQTT报文解析成功(含JSON失败): 按完整长度消费 */
    if (!isKnownType || lenBytes == 0xFF || consumedLen == 0)
        ESP8266_Consume(1);
    else if (result != 0)
        ESP8266_Consume((uint16_t)(1 + lenBytes));
    else
        ESP8266_Consume(consumedLen);

    // 未知报文类型, 直接返回
    if (result == -1)
        return;

    // 释放CMD或PUBLISH的动态内存
    if (type == MQTT_PKT_CMD || type == MQTT_PKT_PUBLISH)
    {
        MQTT_FreeBuffer(cmdid_topic);
        MQTT_FreeBuffer(req_payload);
    }
    /* 释放JSON树: cJSON_Parse成功后必须cJSON_Delete, 否则内存泄漏
     * 每次约300-500字节. STM32F103 RAM仅20KB, 6-7次命令后堆耗尽,
     * cJSON_Parse内部申请失败返回NULL, 导致"payload损坏误报" */
    if (json != NULL)
    {
        cJSON_Delete(json);
        json = NULL;
    }
}
